/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <filesystem>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <map>
#include <set>
#include <stdexcept>
#include <format>
#include <string>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "config.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "managed_pairing_ipc.h"
#include "managed_tls_identity.h"
#include "managed_media_observer.h"
#include "managed_desktop_control.h"
#include "managed_json.h"
#include "managed_revocation.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "system_tray.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"

using namespace std::literals;

namespace nvhttp {

  static constexpr std::string_view EMPTY_PROPERTY_TREE_ERROR_MSG = "Property tree is empty. Probably, control flow got interrupted by an unexpected C++ exception. This is a bug in Sunshine. Moonlight-qt will report Malformed XML (missing root element)."sv;

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  crypto::cert_chain_t cert_chain;

  managed_pairing::tls_identity_registry_t verified_tls_connections;
  std::string certificate_sha256(X509 *certificate) {
    unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned int size = 0;
    if (!certificate || X509_digest(certificate, EVP_sha256(), bytes, &size) != 1 || size != 32) return {};
    static constexpr char HEX[] = "0123456789abcdef"; std::string result;
    for (unsigned int i = 0; i < size; ++i) { result += HEX[bytes[i] >> 4]; result += HEX[bytes[i] & 15]; }
    return result;
  }
  template<class Request> std::string verified_client_sha256(const Request &request) {
    const auto connection = request->connection_identity();
    return verified_tls_connections.find(connection);
  }

  class SunshineHTTPSServer: public SimpleWeb::ServerBase<SunshineHTTPS> {
  public:
    SunshineHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<SunshineHTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      // Disabling TLS 1.0 and 1.1 (see RFC 8996)
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    std::function<int(SSL *)> verify;
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;

  protected:
    boost::asio::ssl::context context;

    void after_bind() override {
      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int verified, boost::asio::ssl::verify_context &ctx) {
          // To respond with an error message, a connection must be established
          return 1;
        });
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }

        if (ec != SimpleWeb::error::operation_aborted) {
          this->accept();
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }
            if (!ec) {
              if (verify && !verify(session->connection->socket->native_handle())) {
                this->write(session, on_verify_failed);
              } else {
                if (verify) {
                  crypto::x509_t peer {SSL_get1_peer_certificate(session->connection->socket->native_handle())};
                  const auto fingerprint = certificate_sha256(peer.get());
                  verified_tls_connections.remember(session->connection, fingerprint);
                }
                this->read(session);
              }
            } else if (this->on_error) {
              this->on_error(session->request, ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

  using https_server_t = SunshineHTTPSServer;
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  struct conf_intern_t {
    std::string servercert;
    std::string pkey;
  } conf_intern;

  struct named_cert_t {
    std::string name;
    std::string uuid;
    std::string cert;
    bool enabled = true;
  };

  struct client_t {
    std::vector<named_cert_t> named_devices;
  };

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t> map_id_sess;
  // HTTP, HTTPS and the optional local IPC share this exact registry. Recursive
  // because fail_pair/remove_session are also used by standalone crypto tests.
  std::recursive_mutex pairing_mutex;
  std::atomic_bool local_pairing_authority {false};
  constexpr auto LOCAL_PAIRING_TTL = std::chrono::seconds(120);
  constexpr std::size_t MAX_LOCAL_PAIRING_REQUESTS = 8;
  std::deque<managed_pairing::request_t> recent_pairing_results;
  client_t client_root;
  std::atomic<uint32_t> session_id_counter;

  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

  std::string get_arg(const args_t &args, const char *name, const char *default_value = nullptr) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  void save_state() {
    pt::ptree root;

    if (fs::exists(config::nvhttp.file_state)) {
      try {
        pt::read_json(config::nvhttp.file_state, root);
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();
        return;
      }
    }

    root.erase("root"s);

    root.put("root.uniqueid", http::unique_id);
    client_t &client = client_root;
    pt::ptree node;

    pt::ptree named_cert_nodes;
    for (auto &named_cert : client.named_devices) {
      pt::ptree named_cert_node;
      named_cert_node.put("name"s, named_cert.name);
      named_cert_node.put("cert"s, named_cert.cert);
      named_cert_node.put("uuid"s, named_cert.uuid);
      named_cert_node.put("enabled"s, named_cert.enabled);
      named_cert_nodes.push_back(std::make_pair(""s, named_cert_node));
    }
    root.add_child("root.named_devices"s, named_cert_nodes);

    try {
      pt::write_json(config::nvhttp.file_state, root);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't write "sv << config::nvhttp.file_state << ": "sv << e.what();
      return;
    }
  }

  void load_state() {
    if (!fs::exists(config::nvhttp.file_state)) {
      BOOST_LOG(info) << "File "sv << config::nvhttp.file_state << " doesn't exist"sv;
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }

    pt::ptree tree;
    try {
      pt::read_json(config::nvhttp.file_state, tree);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();

      return;
    }

    auto unique_id_p = tree.get_optional<std::string>("root.uniqueid");
    if (!unique_id_p) {
      // This file doesn't contain moonlight credentials
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }
    http::unique_id = std::move(*unique_id_p);

    auto root = tree.get_child("root");
    client_t client;

    // Import from old format
    if (root.get_child_optional("devices")) {
      auto device_nodes = root.get_child("devices");
      for (auto &[_, device_node] : device_nodes) {
        auto uniqID = device_node.get<std::string>("uniqueid");

        if (device_node.count("certs")) {
          for (auto &[_, el] : device_node.get_child("certs")) {
            named_cert_t named_cert;
            named_cert.name = ""s;
            named_cert.cert = el.get_value<std::string>();
            named_cert.uuid = uuid_util::uuid_t::generate().string();
            client.named_devices.emplace_back(named_cert);
          }
        }
      }
    }

    if (root.count("named_devices")) {
      for (auto &[_, el] : root.get_child("named_devices")) {
        named_cert_t named_cert;
        named_cert.name = el.get_child("name").get_value<std::string>();
        named_cert.cert = el.get_child("cert").get_value<std::string>();
        named_cert.uuid = el.get_child("uuid").get_value<std::string>();
        named_cert.enabled = el.get<bool>("enabled", true);
        client.named_devices.emplace_back(named_cert);
      }
    }

    // Empty certificate chain and import certs from file
    cert_chain.clear();
    for (auto &named_cert : client.named_devices) {
      cert_chain.add(crypto::x509(named_cert.cert));
    }

    client_root = client;
  }

  void add_authorized_client(const std::string &name, std::string &&cert) {
    client_t &client = client_root;
    named_cert_t named_cert;
    named_cert.name = name;
    named_cert.cert = std::move(cert);
    named_cert.uuid = uuid_util::uuid_t::generate().string();
    client.named_devices.emplace_back(named_cert);

    if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
      save_state();
    }
  }

  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(bool host_audio, const args_t &args) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;

    auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
    std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

    launch_session->host_audio = host_audio;
    std::stringstream mode = std::stringstream(get_arg(args, "mode", "0x0x0"));
    // Split mode by the char "x", to populate width/height/fps
    int x = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (x == 0) {
        launch_session->width = atoi(segment.c_str());
      }
      if (x == 1) {
        launch_session->height = atoi(segment.c_str());
      }
      if (x == 2) {
        launch_session->fps = atoi(segment.c_str());
      }
      x++;
    }
    launch_session->unique_id = (get_arg(args, "uniqueid", "unknown"));
    launch_session->appid = (int) util::from_view(get_arg(args, "appid", "unknown"));
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = (int) util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->continuous_audio = util::from_view(get_arg(args, "continuousAudio", "0"));
    launch_session->gcmap = (int) util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));

    // Encrypted RTSP is enabled with client reported corever >= 1
    auto corever = util::from_view(get_arg(args, "corever", "0"));
    if (corever >= 1) {
      launch_session->rtsp_cipher = crypto::cipher::gcm_t {
        launch_session->gcm_key,
        false
      };
      launch_session->rtsp_iv_counter = 0;
    }
    launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;

    // Generate the unique identifiers for this connection that we will send later during RTSP handshake
    unsigned char raw_payload[8];
    RAND_bytes(raw_payload, sizeof(raw_payload));
    launch_session->av_ping_payload = util::hex_vec(raw_payload);
    RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

    launch_session->iv.resize(16);
    uint32_t prepend_iv = util::endian::big<uint32_t>((int) util::from_view(get_arg(args, "rikeyid")));
    auto prepend_iv_p = (uint8_t *) &prepend_iv;
    std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
    return launch_session;
  }

  void remove_session(const pair_session_t &sess) {
    std::lock_guard guard(pairing_mutex);
    const auto it = map_id_sess.find(sess.client.uniqueID);
    // Never erase a newer map entry on behalf of an unrelated temporary object.
    if (it == map_id_sess.end() || &it->second != &sess) return;
    if (!sess.local_request.id.empty()) {
      auto result = sess.local_request;
      if (result.outcome.empty()) result.outcome = "failed";
      recent_pairing_results.push_back(std::move(result));
      while (recent_pairing_results.size() > 64) recent_pairing_results.pop_front();
    }
    map_id_sess.erase(it);
  }

  /**
   * @brief Persistent revoke of exactly one authorized client certificate.
   *
   * Matching is on the lowercase hex SHA-256 of the stored PEM's DER encoding -
   * the same identity the pairing attempt carried - never on the display name
   * or the locally generated UUID, which the client influences. The surviving
   * records are written back and reloaded, so `cert_chain` no longer verifies
   * the revoked leaf and a reconnect on it fails the unchanged TLS check.
   */
  std::size_t revoke_authorized_certificate(const std::string &fingerprint) {
    if (!managed_revocation::is_fingerprint(fingerprint)) return 0;
    std::size_t removed = 0;
    client_t &client = client_root;
    for (auto it = client.named_devices.begin(); it != client.named_devices.end();) {
      if (managed_revocation::fingerprint_of_pem(it->cert) == fingerprint) {
        it = client.named_devices.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
    if (removed == 0) return 0;
    save_state();
    load_state();
    return removed;
  }

  void write_pairing_reply(pair_session_t &sess, const pt::ptree &tree) {
    std::ostringstream data;
    pt::write_xml(data, tree);
    auto &response = sess.async_insert_pin.response;
    if (response.has_left() && response.left()) {
      response.left()->close_connection_after_response = true;
      response.left()->write(data.str());
    } else if (response.has_right() && response.right()) {
      response.right()->close_connection_after_response = true;
      response.right()->write(data.str());
    }
    response = std::decay_t<decltype(response.left())>();
  }

  void expire_managed_pairing_requests() {
    std::lock_guard guard(pairing_mutex);
    const auto now = managed_pairing::request_t::clock::now();
    for (auto it = map_id_sess.begin(); it != map_id_sess.end();) {
      auto &sess = it->second;
      if (!sess.local_request.id.empty() && sess.local_request.expired(now)) {
        sess.local_request.outcome = "expired";
        pt::ptree tree;
        tree.put("root.paired", 0);
        tree.put("root.<xmlattr>.status_code", 408);
        tree.put("root.<xmlattr>.status_message", "Pairing request expired");
        write_pairing_reply(sess, tree);
        ++it;
        remove_session(sess);
      } else ++it;
    }
    while (!recent_pairing_results.empty() && now > recent_pairing_results.front().deadline + LOCAL_PAIRING_TTL)
      recent_pairing_results.pop_front();
  }

  void fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string status_msg) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", status_msg);
    remove_session(sess);  // Security measure, delete the session when something went wrong and force a re-pair
  }

  void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
    if (sess.last_phase != PAIR_PHASE::NONE) {
      fail_pair(sess, tree, "Out of order call to getservercert");
      return;
    }
    sess.last_phase = PAIR_PHASE::GETSERVERCERT;

    if (sess.async_insert_pin.salt.size() < 32) {
      fail_pair(sess, tree, "Salt too short");
      return;
    }

    std::string_view salt_view {sess.async_insert_pin.salt.data(), 32};

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
    if (sess.last_phase != PAIR_PHASE::GETSERVERCERT) {
      fail_pair(sess, tree, "Out of order call to clientchallenge");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTCHALLENGE;

    if (!sess.cipher_key) {
      fail_pair(sess, tree, "Cipher key not set");
      return;
    }
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({(char *) decrypted.data(), decrypted.size()});
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
    if (sess.last_phase != PAIR_PHASE::CLIENTCHALLENGE) {
      fail_pair(sess, tree, "Out of order call to serverchallengeresp");
      return;
    }
    sess.last_phase = PAIR_PHASE::SERVERCHALLENGERESP;

    if (!sess.cipher_key || sess.serversecret.empty()) {
      fail_pair(sess, tree, "Cipher key or serversecret not set");
      return;
    }

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientpairingsecret(pair_session_t &sess, std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, pt::ptree &tree, const std::string &client_pairing_secret) {
    if (sess.last_phase != PAIR_PHASE::SERVERCHALLENGERESP) {
      fail_pair(sess, tree, "Out of order call to clientpairingsecret");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTPAIRINGSECRET;

    auto &client = sess.client;

    if (client_pairing_secret.size() <= 16) {
      fail_pair(sess, tree, "Client pairing secret too short");
      return;
    }

    std::string_view secret {client_pairing_secret.data(), 16};
    std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

    auto x509 = crypto::x509(client.cert);
    if (!x509) {
      fail_pair(sess, tree, "Invalid client certificate");
      return;
    }
    auto x509_sign = crypto::signature(x509);

    std::string data;
    data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

    data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
    data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
    data.insert(std::end(data), std::begin(secret), std::end(secret));

    auto hash = crypto::hash(data);

    // Managed deadlines are checked again after crypto verification, before a
    // certificate can be authorized; beginning a stage just before expiry does
    // not extend the request. The cryptographic checks themselves are unchanged.
    if (!sess.local_request.id.empty() && sess.local_request.expired()) {
      sess.local_request.outcome = "expired";
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 408);
      remove_session(sess);
      return;
    }
    // if hash not correct, probably MITM
    bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
    auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    if (!sess.local_request.id.empty() && sess.local_request.expired()) {
      sess.local_request.outcome = "expired";
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 408);
      remove_session(sess);
      return;
    }
    if (same_hash && verify) {
      tree.put("root.paired", 1);
      add_cert->raise(crypto::x509(client.cert));

      // The client is now successfully paired and will be authorized to connect
      add_authorized_client(client.name, std::move(client.cert));
      sess.local_request.outcome = "paired";
    } else {
      tree.put("root.paired", 0);
      sess.local_request.outcome = "failed";
    }

    remove_session(sess);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  template<class T>
  struct tunnel;

  template<>
  struct tunnel<SunshineHTTPS> {
    static auto constexpr to_string = "HTTPS"sv;
  };

  template<>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;
  };

  template<class T>
  void print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    BOOST_LOG(debug) << "TUNNEL :: "sv << tunnel<T>::to_string;

    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  template<class T>
  void not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());

    *response
      << "HTTP/1.1 404 NOT FOUND\r\n"
      << data.str();

    response->close_connection_after_response = true;
  }

  template<class T>
  void pair(std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    // Do not log pairing query values (certificate/challenges/salt).
    std::lock_guard guard(pairing_mutex);
    expire_managed_pairing_requests();

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID {get_arg(args, "uniqueid")};

    args_t::const_iterator it;
    if (it = args.find("phrase"); it != std::end(args)) {
      if (it->second == "getservercert"sv) {
        pair_session_t sess;

        sess.client.uniqueID = std::move(uniqID);
        const auto encoded_cert = get_arg(args, "clientcert");
        const auto salt = get_arg(args, "salt");
        if (local_pairing_authority && (sess.client.uniqueID.empty() || sess.client.uniqueID.size() > 128
            || encoded_cert.empty() || encoded_cert.size() > 32768 || salt.size() < 32 || salt.size() > 256)) {
          tree.put("root.paired", 0);
          tree.put("root.<xmlattr>.status_code", 400);
          return;
        }
        sess.client.cert = util::from_hex_vec(encoded_cert, true);
        if (local_pairing_authority) {
          auto certificate = crypto::x509(sess.client.cert);
          unsigned char bytes[EVP_MAX_MD_SIZE];
          unsigned int size = 0;
          if (!certificate || X509_digest(certificate.get(), EVP_sha256(), bytes, &size) != 1 || size != 32) {
            tree.put("root.paired", 0); tree.put("root.<xmlattr>.status_code", 400); return;
          }
          static constexpr char HEX[] = "0123456789abcdef";
          for (unsigned int i = 0; i < size; ++i) {
            sess.local_request.client_cert_sha256 += HEX[bytes[i] >> 4];
            sess.local_request.client_cert_sha256 += HEX[bytes[i] & 15];
          }
          if (map_id_sess.size() >= MAX_LOCAL_PAIRING_REQUESTS || map_id_sess.contains(sess.client.uniqueID)
              || std::any_of(map_id_sess.begin(), map_id_sess.end(), [&](const auto &entry) {
                return entry.second.local_request.client_cert_sha256 == sess.local_request.client_cert_sha256;
              })) {
            tree.put("root.paired", 0); tree.put("root.<xmlattr>.status_code", 409); return;
          }
          sess.local_request.id = uuid_util::uuid_t::generate().string();
          sess.local_request.deadline = managed_pairing::request_t::clock::now() + LOCAL_PAIRING_TTL;
        }
        auto ptr = map_id_sess.emplace(sess.client.uniqueID, std::move(sess)).first;
        ptr->second.async_insert_pin.salt = salt;
        if (config::sunshine.flags[config::flag::PIN_STDIN]) {
          std::string pin;

          std::cout << "Please insert pin: "sv;
          std::getline(std::cin, pin);

          getservercert(ptr->second, tree, pin);
          return;
        } else {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
          system_tray::update_tray_require_pin();
#endif
          ptr->second.async_insert_pin.response = std::move(response);

          fg.disable();
          return;
        }
      } else if (it->second == "pairchallenge"sv) {
        tree.put("root.paired", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        return;
      }
    }

    auto sess_it = map_id_sess.find(uniqID);
    if (sess_it == std::end(map_id_sess)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");

      return;
    }

    if (it = args.find("clientchallenge"); it != std::end(args)) {
      auto challenge = util::from_hex_vec(it->second, true);
      clientchallenge(sess_it->second, tree, challenge);
    } else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
      auto encrypted_response = util::from_hex_vec(it->second, true);
      serverchallengeresp(sess_it->second, tree, encrypted_response);
    } else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
      auto pairingsecret = util::from_hex_vec(it->second, true);
      clientpairingsecret(sess_it->second, add_cert, tree, pairingsecret);
    } else {
      tree.put("root.<xmlattr>.status_code", 404);
      tree.put("root.<xmlattr>.status_message", "Invalid pairing request");
    }
  }

  bool submit_pin(pair_session_t &sess, const std::string &pin, const std::string &name) {
    if (!managed_pairing::request_t::valid_pin(pin) || sess.last_phase != PAIR_PHASE::NONE) return false;
    auto &held = sess.async_insert_pin.response;
    if (!(held.has_left() && held.left()) && !(held.has_right() && held.right())) return false;
    // Move the response before crypto validation: fail_pair may erase sess.
    auto response = std::move(held);
    held = std::decay_t<decltype(held.left())>();
    sess.client.name = name;
    sess.local_request.pin_submitted = true;
    pt::ptree tree;
    getservercert(sess, tree, pin);
    std::ostringstream data;
    pt::write_xml(data, tree);
    if (response.has_left() && response.left()) response.left()->write(data.str());
    else if (response.has_right() && response.right()) response.right()->write(data.str());
    return tree.get<int>("root.paired", 0) == 1;
  }

  bool pin(std::string pin, std::string name) {
    std::lock_guard guard(pairing_mutex);
    // Managed mode has no ambiguous network/Web approval fallback. Stock
    // clients still use the unchanged GameStream challenge exchange.
    if (local_pairing_authority || map_id_sess.empty()) return false;
    return submit_pin(std::begin(map_id_sess)->second, pin, name);
  }

  nlohmann::json managed_pairing_control(const nlohmann::json &request) {
    std::lock_guard guard(pairing_mutex);
    const auto error = [](const char *code) { return nlohmann::json {{"ok", false}, {"error", {{"code", code}}}}; };
    if (!local_pairing_authority) return error("local_pairing_disabled");
    expire_managed_pairing_requests();
    if (!request.is_object() || !request.contains("op") || !request["op"].is_string()) return error("invalid_request");
    const auto op = request["op"].get<std::string>();
    if (op.starts_with("desktop.")) return managed_desktop::control().request(request,
      config::video.capture == "wlr", config::video.encoder == "vaapi", video::active_hevc_mode >= 2);
    if (op == "media.list") {
      if (request.size() != 1) return error("invalid_request");
      return managed_media::registry().list();
    }
    if (op == "media.get") {
      if (request.size() != 2 || !request.contains("session_id") || !request["session_id"].is_string()) return error("invalid_request");
      return managed_media::registry().get(request["session_id"].get<std::string>());
    }
    const auto metadata = [](const managed_pairing::request_t &item) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(item.deadline - managed_pairing::request_t::clock::now()).count();
      return nlohmann::json {{"request_id", item.id}, {"client_cert_sha256", item.client_cert_sha256},
        {"status", item.status()}, {"expires_in_ms", std::max<std::int64_t>(0, remaining)}};
    };
    if (op == "pairing.list") {
      if (request.size() != 1) return error("invalid_request");
      nlohmann::json rows = nlohmann::json::array();
      for (const auto &[_, sess] : map_id_sess) if (!sess.local_request.id.empty()) rows.push_back(metadata(sess.local_request));
      for (const auto &item : recent_pairing_results) rows.push_back(metadata(item));
      // The capability answer for `pairing.revoke`. A caller that does not see
      // it must keep treating a revoke as unresolved, exactly as before.
      return {{"ok", true}, {"requests", rows}, {"certificate_revocation_supported", true}};
    }
    if (op == "pairing.clients") {
      // The authorized client store, by the only identity a revoke can use.
      // No certificate, key material or PEM leaves this socket - a fingerprint
      // and the display name the record already carries, nothing else.
      if (request.size() != 1) return error("invalid_request");
      nlohmann::json rows = nlohmann::json::array();
      std::size_t unreadable = 0;
      bool truncated = false;
      for (const auto &named_cert : client_root.named_devices) {
        const auto fingerprint = managed_revocation::fingerprint_of_pem(named_cert.cert);
        if (fingerprint.empty()) { ++unreadable; continue; }
        if (rows.size() >= 64) { truncated = true; break; }
        std::string name;
        for (const char c : named_cert.name) {
          if (name.size() >= 80) break;
          if (static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) != 127) name += c;
        }
        rows.push_back({{"client_cert_sha256", fingerprint}, {"name", name}, {"enabled", named_cert.enabled}});
      }
      return {{"ok", true}, {"clients", rows}, {"unreadable", unreadable}, {"truncated", truncated}};
    }
    if (op == "pairing.revoke") {
      // Persistent revoke. It is deliberately addressed by certificate alone:
      // the request ID belongs to a temporary attempt that is long gone by the
      // time a user revokes a device, and the certificate is what still
      // authorizes a stream.
      if (request.size() != 2 || !request.contains("client_cert_sha256") || !request["client_cert_sha256"].is_string()) return error("invalid_request");
      const auto revoked = request["client_cert_sha256"].get<std::string>();
      if (!managed_revocation::is_fingerprint(revoked)) return error("invalid_request");
      // Cancel any temporary attempt for the same certificate first: a revoke
      // that left one in flight would let it finish into a fresh authorization.
      std::vector<std::string> attempts;
      for (const auto &[unique_id, entry] : map_id_sess) {
        if (entry.local_request.client_cert_sha256 == revoked) attempts.push_back(unique_id);
      }
      for (const auto &unique_id : attempts) {
        const auto found = map_id_sess.find(unique_id);
        if (found == map_id_sess.end()) continue;
        found->second.local_request.outcome = "cancelled";
        pt::ptree tree; tree.put("root.paired", 0); tree.put("root.<xmlattr>.status_code", 410);
        write_pairing_reply(found->second, tree); remove_session(found->second);
      }
      const auto removed = revoke_authorized_certificate(revoked);
      // Only the streams this certificate authorized; never an unrelated
      // session and never a host quit.
      const auto stopped = managed_media::registry().request_stop_for_client(revoked);
      // Counts only. The fingerprint is an identity and is never logged, like
      // every other value on this socket.
      BOOST_LOG(info) << "Managed pairing: revoke removed "sv << removed
                      << " authorized certificate(s), cancelled "sv << attempts.size()
                      << " attempt(s), signalled "sv << stopped << " stream(s)"sv;
      return {{"ok", true}, {"status", removed > 0 ? "revoked" : "not_found"}};
    }
    if (op != "pairing.pending" && op != "pairing.approve" && op != "pairing.cancel") return error("unknown_operation");
    const std::size_t expected_fields = op == "pairing.approve" ? 5 : 3;
    if (request.size() != expected_fields || !request.contains("request_id") || !request["request_id"].is_string()
        || !request.contains("client_cert_sha256") || !request["client_cert_sha256"].is_string()) return error("invalid_request");
    const auto id = request["request_id"].get<std::string>();
    const auto fingerprint = request["client_cert_sha256"].get<std::string>();
    if (id.size() != 36 || fingerprint.size() != 64) return error("invalid_request");
    auto found = std::find_if(map_id_sess.begin(), map_id_sess.end(), [&](const auto &entry) { return entry.second.local_request.id == id; });
    if (found == map_id_sess.end()) {
      const auto recent = std::find_if(recent_pairing_results.begin(), recent_pairing_results.end(), [&](const auto &entry) {
        return entry.id == id && entry.client_cert_sha256 == fingerprint;
      });
      if (recent != recent_pairing_results.end() && op == "pairing.pending")
        return {{"ok", true}, {"request", metadata(*recent)}, {"certificate_revocation_supported", true}};
      return error("pairing_request_not_pending");
    }
    auto &sess = found->second;
    if (sess.local_request.client_cert_sha256 != fingerprint) return error("pairing_binding_mismatch");
    if (op == "pairing.pending") return {{"ok", true}, {"request", metadata(sess.local_request)}, {"certificate_revocation_supported", true}};
    if (op == "pairing.cancel") {
      sess.local_request.outcome = "cancelled";
      auto result = metadata(sess.local_request);
      pt::ptree tree; tree.put("root.paired", 0); tree.put("root.<xmlattr>.status_code", 410);
      write_pairing_reply(sess, tree); remove_session(sess);
      return {{"ok", true}, {"request", result}};
    }
    if (!request.contains("pin") || !request["pin"].is_string() || !request.contains("name") || !request["name"].is_string()) return error("invalid_request");
    const auto pin = request["pin"].get<std::string>();
    const auto name = request["name"].get<std::string>();
    if (!managed_pairing::request_t::valid_pin(pin) || name.size() > 80
        || std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; })) return error("invalid_request");
    const auto rejected = sess.local_request.can_approve(id, fingerprint);
    if (!rejected.empty()) return error(rejected.c_str());
    if (!submit_pin(sess, pin, name)) return error("pairing_pin_not_submitted");
    // No certificate is authorized by this response. Only unchanged upstream
    // clientpairingsecret verification can create a paired result.
    return {{"ok", true}, {"accepted", true}, {"request_id", id}, {"status", "awaiting_client_proof"}};
  }

  template<class T>
  void serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    int pair_status = 0;
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      auto args = request->parse_query_string();
      auto clientID = args.find("uniqueid"s);

      if (clientID != std::end(args)) {
        pair_status = 1;
      }
    }

    auto local_endpoint = request->local_endpoint();

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", video::active_hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    } else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    uint32_t codec_mode_flags = SCM_H264;
    if (video::last_encoder_probe_supported_yuv444_for_codec[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (video::active_hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if (video::active_hevc_mode >= 3) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT10_444;
      }
    }
    if (video::active_av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH8_444;
      }
    }
    if (video::active_av1_mode >= 3) {
      codec_mode_flags |= SCM_AV1_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH10_444;
      }
    }
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);

    if (!config::nvhttp.external_ip.empty()) {
      tree.put("root.ExternalIP", config::nvhttp.external_ip);
    }

    auto current_appid = proc::proc.running();
    tree.put("root.PairStatus", pair_status);
    tree.put("root.currentgame", current_appid);
    tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  nlohmann::json get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    client_t &client = client_root;
    for (auto &named_cert : client.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert.name;
      named_cert_node["uuid"] = named_cert.uuid;
      named_cert_node["enabled"] = named_cert.enabled;
      named_cert_nodes.push_back(named_cert_node);
    }

    return named_cert_nodes;
  }

  void applist(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    apps.put("<xmlattr>.status_code", 200);

    for (auto &proc : proc::proc.get_apps()) {
      pt::ptree app;

      app.put("IsHdrSupported"s, video::active_hevc_mode == 3 ? 1 : 0);
      app.put("AppTitle"s, proc.name);
      app.put("ID", proc.id);

      apps.push_back(std::make_pair("App", std::move(app)));
    }
  }

  void launch(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    bool revert_display_configuration {false};
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;

      if (revert_display_configuration) {
        display_device::revert_configuration();
      }
    });

    const auto client_fingerprint = verified_client_sha256(request);
    if (client_fingerprint.empty() || !managed_desktop::control().admit_launch(client_fingerprint)) {
      tree.put("root.resume", 0); tree.put("root.<xmlattr>.status_code", 409); return;
    }
    auto abandon_managed = util::fail_guard([&]() { managed_desktop::control().abandon_launch(client_fingerprint); });
    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      args.find("appid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    auto appid = util::from_view(get_arg(args, "appid"));

    auto current_appid = proc::proc.running();
    if (current_appid > 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

      return;
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    auto launch_session = make_launch_session(host_audio, args);
    launch_session->managed_client_cert_sha256 = client_fingerprint;

    if (rtsp_stream::session_count() == 0) {
      // The display should be restored in case something fails as there are no other sessions.
      revert_display_configuration = true;

      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      display_device::configure_display(config::video, *launch_session);

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    if (appid > 0) {
      auto err = proc::proc.execute((int) appid, launch_session);
      if (err) {
        tree.put("root.<xmlattr>.status_code", err);
        tree.put("root.<xmlattr>.status_message", "Failed to start the specified application");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.gamesession", 1);

    rtsp_stream::launch_session_raise(launch_session);
    abandon_managed.disable();

    // Stream was started successfully, we will revert the config when the app or session terminates
    revert_display_configuration = false;
  }

  void resume(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto current_appid = proc::proc.running();
    if (current_appid == 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

      return;
    }

    const auto client_fingerprint = verified_client_sha256(request);
    if (client_fingerprint.empty() || !managed_desktop::control().admit_launch(client_fingerprint)) {
      tree.put("root.resume", 0); tree.put("root.<xmlattr>.status_code", 409); return;
    }
    auto abandon_managed = util::fail_guard([&]() { managed_desktop::control().abandon_launch(client_fingerprint); });
    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    // Newer Moonlight clients send localAudioPlayMode on /resume too,
    // so we should use it if it's present in the args and there are
    // no active sessions we could be interfering with.
    const bool no_active_sessions {rtsp_stream::session_count() == 0};
    if (no_active_sessions && args.find("localAudioPlayMode"s) != std::end(args)) {
      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    }
    const auto launch_session = make_launch_session(host_audio, args);
    launch_session->managed_client_cert_sha256 = client_fingerprint;

    if (no_active_sessions) {
      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      display_device::configure_display(config::video, *launch_session);

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.resume", 1);

    rtsp_stream::launch_session_raise(launch_session);
    abandon_managed.disable();
  }

  void cancel(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    rtsp_stream::terminate_sessions();

    if (proc::proc.running() > 0) {
      proc::proc.terminate();
    }

    // The config needs to be reverted regardless of whether "proc::proc.terminate()" was called or not.
    display_device::revert_configuration();
  }

  void appasset(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto args = request->parse_query_string();
    auto app_image = proc::proc.get_app_image((int) util::from_view(get_arg(args, "appid")));

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
  }

  bool is_client_enabled(const std::string_view cert_pem);

  void start() {
    platf::set_thread_name("nvhttp");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      load_state();
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    auto add_cert = std::make_shared<safe::queue_t<crypto::x509_t>>(30);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;

    // Verify certificates after establishing connection
    https_server.verify = [add_cert](SSL *ssl) {
      crypto::x509_t x509 {
#if OPENSSL_VERSION_MAJOR >= 3
        SSL_get1_peer_certificate(ssl)
#else
        SSL_get_peer_certificate(ssl)
#endif
      };
      if (!x509) {
        BOOST_LOG(info) << "unknown -- denied"sv;
        return 0;
      }

      int verified = 0;

      auto fg = util::fail_guard([&]() {
        char subject_name[256];

        X509_NAME_oneline(X509_get_subject_name(x509.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(debug) << subject_name << " -- "sv << (verified ? "verified"sv : "denied"sv);
      });

      while (add_cert->peek()) {
        char subject_name[256];

        auto cert = add_cert->pop();
        X509_NAME_oneline(X509_get_subject_name(cert.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(debug) << "Added cert ["sv << subject_name << ']';
        cert_chain.add(std::move(cert));
      }

      auto err_str = cert_chain.verify(x509.get());
      if (err_str) {
        BOOST_LOG(warning) << "SSL Verification error :: "sv << err_str;

        return verified;
      }

      // Check if this client is enabled
      auto pem = crypto::pem(x509);
      if (!is_client_enabled(pem)) {
        BOOST_LOG(info) << "Client is disabled -- denied"sv;
        return verified;
      }

      verified = 1;

      return verified;
    };

    https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
      pt::ptree tree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        resp->write(data.str());
        resp->close_connection_after_response = true;
      });

      tree.put("root.<xmlattr>.status_code"s, 401);
      tree.put("root.<xmlattr>.query"s, req->path);
      tree.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);
    };

    https_server.default_resource["GET"] = not_found<SunshineHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = serverinfo<SunshineHTTPS>;
    https_server.resource["^/pair$"]["GET"] = [&add_cert](auto resp, auto req) {
      pair<SunshineHTTPS>(add_cert, resp, req);
    };
    https_server.resource["^/applist$"]["GET"] = applist;
    https_server.resource["^/appasset$"]["GET"] = appasset;
    https_server.resource["^/launch$"]["GET"] = [&host_audio](auto resp, auto req) {
      launch(host_audio, resp, req);
    };
    https_server.resource["^/resume$"]["GET"] = [&host_audio](auto resp, auto req) {
      resume(host_audio, resp, req);
    };
    https_server.resource["^/cancel$"]["GET"] = cancel;

    https_server.config.reuse_address = true;
    https_server.config.address = net::get_bind_address(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = [&add_cert](auto resp, auto req) {
      pair<SimpleWeb::HTTP>(add_cert, resp, req);
    };

    http_server.config.reuse_address = true;
    http_server.config.address = net::get_bind_address(address_family);
    http_server.config.port = port_http;

    auto accept_and_run = [&](auto *http_server) {
      try {
        std::string name = "nvhttp::" + std::to_string(http_server->config.port);
        platf::set_thread_name(name);
        http_server->start();
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    managed_pairing::local_server_t local_pairing_server(
      [](const std::string &line) {
        try {
          return managed_pairing_control(managed_json::parse_object_keys(line)).dump();
        }
        catch (...) { return std::string {R"({"ok":false,"error":{"code":"invalid_request"}})"}; }
      }, expire_managed_pairing_requests);
    const auto managed = std::getenv("SUNSHINE_MANAGED_LOCAL_PAIRING");
    if (managed && std::string_view(managed) == "1") {
      local_pairing_authority = true;
      const auto runtime = std::getenv("XDG_RUNTIME_DIR");
      if (config::sunshine.flags[config::flag::PIN_STDIN] || !runtime || !local_pairing_server.start(runtime)) {
        BOOST_LOG(error) << "Managed local pairing could not start; GameStream listener not opened";
        shutdown_event->raise(true);
        return;
      }
    }
    managed_media::registry().enable(local_pairing_authority);
    std::thread ssl {accept_and_run, &https_server};
    std::thread tcp {accept_and_run, &http_server};

    // Wait for any event
    shutdown_event->view();

    local_pairing_server.stop();
    {
      std::lock_guard guard(pairing_mutex);
      for (auto &[_, sess] : map_id_sess) {
        pt::ptree tree; tree.put("root.paired", 0); tree.put("root.<xmlattr>.status_code", 503);
        write_pairing_reply(sess, tree);
      }
      map_id_sess.clear();
      recent_pairing_results.clear();
    }
    local_pairing_authority = false;
    managed_media::registry().enable(false);
    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
  }

  void erase_all_clients() {
    client_t client;
    client_root = client;
    cert_chain.clear();
    save_state();
  }

  bool unpair_client(const std::string_view uuid) {
    bool removed = false;
    client_t &client = client_root;
    for (auto it = client.named_devices.begin(); it != client.named_devices.end();) {
      if ((*it).uuid == uuid) {
        it = client.named_devices.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }

    save_state();
    load_state();
    return removed;
  }

  bool set_client_enabled(const std::string_view uuid, bool enabled) {
    client_t &client = client_root;
    for (auto &named_cert : client.named_devices) {
      if (named_cert.uuid == uuid) {
        named_cert.enabled = enabled;
        save_state();
        return true;
      }
    }
    return false;
  }

  bool is_client_enabled(const std::string_view cert_pem) {
    const client_t &client = client_root;
    for (const auto &named_cert : client.named_devices) {
      if (named_cert.cert == cert_pem) {
        return named_cert.enabled;
      }
    }
    return true;
  }
}  // namespace nvhttp
