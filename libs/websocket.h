// websocket.h — Minimal WebSocket client with auto-reconnect.
// Uses POSIX sockets + OpenSSL for TLS. Header-only, no exceptions.
//
// Implements RFC 6455: frame masking, ping/pong, close handshake,
// fragmented messages, and exponential backoff reconnection.

#pragma once

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ws {

enum class MessageType { Open, Close, Message, Error };

struct Message {
  MessageType type{};
  std::string data{};
  bool binary = false;
  std::string error_reason{};
  std::string close_reason{};
};

using MessageCallback = std::function<void(const Message &)>;

class Client {
 public:
  Client() = default;
  ~Client() { stop(); }

  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  void setUrl(const std::string &url) {
    size_t pos = url.find("://");
    if (pos == std::string::npos) return;
    std::string scheme = url.substr(0, pos);
    use_tls_ = (scheme == "wss");

    size_t host_start = pos + 3;
    size_t path_start = url.find('/', host_start);
    std::string host_port;
    if (path_start != std::string::npos) {
      host_port = url.substr(host_start, path_start - host_start);
      path_ = url.substr(path_start);
    } else {
      host_port = url.substr(host_start);
      path_ = "/";
    }

    size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
      host_ = host_port.substr(0, colon);
      port_ = std::stoi(host_port.substr(colon + 1));
    } else {
      host_ = host_port;
      port_ = use_tls_ ? 443 : 80;
    }
  }

  void enableAutomaticReconnection() { auto_reconnect_ = true; }

  void setOnMessageCallback(MessageCallback cb) { callback_ = std::move(cb); }

  void start() {
    if (running_) return;
    // Ignore SIGPIPE so broken connections don't kill the process.
    // This is standard practice for networked daemons and is safe
    // alongside the app's SIGTERM/SIGINT handlers.
    signal(SIGPIPE, SIG_IGN);
    running_ = true;
    thread_ = std::thread(&Client::run, this);
  }

  void stop() {
    if (!running_) return;
    running_ = false;
    {
      std::lock_guard<std::mutex> lock(send_mutex_);
      if (sockfd_ >= 0) {
        ::shutdown(sockfd_, SHUT_RDWR);
      }
    }
    if (thread_.joinable()) thread_.join();
  }

  void send(const std::string &data, bool binary = false) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!connected_) return;
    sendFrame(binary ? 0x02 : 0x01,
              reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }

 private:
  std::string host_;
  std::string path_ = "/";
  int port_ = 0;
  bool use_tls_ = false;

  int sockfd_ = -1;
  SSL_CTX *ssl_ctx_ = nullptr;
  SSL *ssl_ = nullptr;

  std::thread thread_;
  std::mutex send_mutex_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};

  bool auto_reconnect_ = false;
  static constexpr uint32_t kMinReconnectMs = 100;
  static constexpr uint32_t kMaxReconnectMs = 10000;

  MessageCallback callback_;

  // --- Connection management ---

  bool tcpConnect() {
    struct addrinfo hints {}, *result;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result) != 0)
      return false;

    for (auto *rp = result; rp; rp = rp->ai_next) {
      sockfd_ = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (sockfd_ < 0) continue;
      if (::connect(sockfd_, rp->ai_addr, rp->ai_addrlen) == 0) {
        freeaddrinfo(result);
        return true;
      }
      ::close(sockfd_);
      sockfd_ = -1;
    }

    freeaddrinfo(result);
    return false;
  }

  bool tlsConnect() {
    ssl_ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx_) return false;

    ssl_ = SSL_new(ssl_ctx_);
    if (!ssl_) return false;

    SSL_set_fd(ssl_, sockfd_);
    SSL_set_tlsext_host_name(ssl_, host_.c_str());

    return SSL_connect(ssl_) == 1;
  }

  void disconnect() {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (ssl_) {
      SSL_shutdown(ssl_);
      SSL_free(ssl_);
      ssl_ = nullptr;
    }
    if (ssl_ctx_) {
      SSL_CTX_free(ssl_ctx_);
      ssl_ctx_ = nullptr;
    }
    if (sockfd_ >= 0) {
      ::close(sockfd_);
      sockfd_ = -1;
    }
  }

  // --- Low-level I/O ---

  bool rawSend(const void *data, size_t len) {
    auto *ptr = static_cast<const uint8_t *>(data);
    while (len > 0) {
      ssize_t n;
      if (ssl_) {
        n = SSL_write(ssl_, ptr, static_cast<int>(len));
      } else {
        n = ::send(sockfd_, ptr, len, MSG_NOSIGNAL);
      }
      if (n <= 0) return false;
      ptr += n;
      len -= static_cast<size_t>(n);
    }
    return true;
  }

  // rawRecv reads from the socket without holding send_mutex_.
  // Only the receive thread calls this, so no locking is needed.
  bool rawRecv(void *data, size_t len) {
    auto *ptr = static_cast<uint8_t *>(data);
    while (len > 0) {
      ssize_t n;
      if (ssl_) {
        n = SSL_read(ssl_, ptr, static_cast<int>(len));
      } else {
        n = ::recv(sockfd_, ptr, len, 0);
      }
      if (n <= 0) return false;
      ptr += n;
      len -= static_cast<size_t>(n);
    }
    return true;
  }

  // --- WebSocket handshake ---

  static std::string base64Encode(const uint8_t *data, int len) {
    // EVP_EncodeBlock output size: 4 * ceil(len/3) + 1
    std::string result(4 * ((len + 2) / 3) + 1, '\0');
    int actual = EVP_EncodeBlock(
        reinterpret_cast<unsigned char *>(result.data()), data, len);
    result.resize(static_cast<size_t>(actual));
    return result;
  }

  bool performHandshake() {
    uint8_t key_bytes[16];
    RAND_bytes(key_bytes, sizeof(key_bytes));
    std::string key = base64Encode(key_bytes, sizeof(key_bytes));

    std::string request = "GET " + path_ +
                          " HTTP/1.1\r\n"
                          "Host: " +
                          host_ +
                          "\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Key: " +
                          key +
                          "\r\n"
                          "Sec-WebSocket-Version: 13\r\n"
                          "\r\n";

    if (!rawSend(request.data(), request.size())) return false;

    // Read response until \r\n\r\n
    std::string response;
    char c;
    while (rawRecv(&c, 1)) {
      response += c;
      if (response.size() >= 4 &&
          response.compare(response.size() - 4, 4, "\r\n\r\n") == 0) {
        break;
      }
      if (response.size() > 4096) return false;
    }

    return response.find("HTTP/1.1 101") != std::string::npos;
  }

  // --- WebSocket framing ---

  // sendFrame must be called with send_mutex_ held (or from rawSend context).
  bool sendFrame(uint8_t opcode, const uint8_t *data, size_t len) {
    // Build header into a fixed-size buffer (max 14 bytes: 2 base + 8 ext + 4 mask)
    uint8_t header[14];
    size_t hlen = 0;

    header[hlen++] = 0x80 | opcode;  // FIN + opcode

    // Payload length with mask bit set (client must mask)
    if (len < 126) {
      header[hlen++] = 0x80 | static_cast<uint8_t>(len);
    } else if (len <= 0xFFFF) {
      header[hlen++] = 0x80 | 126;
      header[hlen++] = static_cast<uint8_t>((len >> 8) & 0xFF);
      header[hlen++] = static_cast<uint8_t>(len & 0xFF);
    } else {
      header[hlen++] = 0x80 | 127;
      for (int i = 7; i >= 0; i--) {
        header[hlen++] = static_cast<uint8_t>((len >> (8 * i)) & 0xFF);
      }
    }

    // Masking key
    uint8_t mask[4];
    RAND_bytes(mask, sizeof(mask));
    std::memcpy(header + hlen, mask, 4);
    hlen += 4;

    if (!rawSend(header, hlen)) return false;

    // Send masked payload
    if (len > 0) {
      std::vector<uint8_t> masked(len);
      for (size_t i = 0; i < len; i++) {
        masked[i] = data[i] ^ mask[i % 4];
      }
      return rawSend(masked.data(), masked.size());
    }
    return true;
  }

  bool readFrame(bool &fin, uint8_t &opcode, std::vector<uint8_t> &payload) {
    uint8_t header[2];
    if (!rawRecv(header, 2)) return false;

    fin = (header[0] & 0x80) != 0;
    opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;

    if (payload_len == 126) {
      uint8_t ext[2];
      if (!rawRecv(ext, 2)) return false;
      payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
      uint8_t ext[8];
      if (!rawRecv(ext, 8)) return false;
      payload_len = 0;
      for (int i = 0; i < 8; i++) {
        payload_len = (payload_len << 8) | ext[i];
      }
    }

    // Sanity limit: 64 MB
    if (payload_len > 64 * 1024 * 1024) return false;

    uint8_t mask[4] = {};
    if (masked) {
      if (!rawRecv(mask, 4)) return false;
    }

    payload.resize(static_cast<size_t>(payload_len));
    if (payload_len > 0) {
      if (!rawRecv(payload.data(), static_cast<size_t>(payload_len)))
        return false;
      if (masked) {
        for (size_t i = 0; i < payload_len; i++) {
          payload[i] ^= mask[i % 4];
        }
      }
    }

    return true;
  }

  // --- Main thread loop ---

  void run() {
    uint32_t backoff_ms = kMinReconnectMs;

    while (running_) {
      if (!tcpConnect()) {
        fireError("Connection failed to " + host_ + ":" +
                  std::to_string(port_));
        if (!running_ || !auto_reconnect_) break;
        sleepMs(backoff_ms);
        backoff_ms = std::min(backoff_ms * 2, kMaxReconnectMs);
        continue;
      }

      if (use_tls_ && !tlsConnect()) {
        fireError("TLS handshake failed");
        disconnect();
        if (!running_ || !auto_reconnect_) break;
        sleepMs(backoff_ms);
        backoff_ms = std::min(backoff_ms * 2, kMaxReconnectMs);
        continue;
      }

      if (!performHandshake()) {
        fireError("WebSocket handshake failed");
        disconnect();
        if (!running_ || !auto_reconnect_) break;
        sleepMs(backoff_ms);
        backoff_ms = std::min(backoff_ms * 2, kMaxReconnectMs);
        continue;
      }

      // Successfully connected
      connected_ = true;
      backoff_ms = kMinReconnectMs;
      if (callback_) {
        callback_({.type = MessageType::Open});
      }

      // Receive loop
      std::string close_reason;
      receiveLoop(close_reason);

      connected_ = false;
      if (callback_) {
        callback_({.type = MessageType::Close, .close_reason = close_reason});
      }
      disconnect();

      if (!running_ || !auto_reconnect_) break;
      sleepMs(backoff_ms);
      backoff_ms = std::min(backoff_ms * 2, kMaxReconnectMs);
    }
  }

  void receiveLoop(std::string &close_reason) {
    std::vector<uint8_t> message_buffer;
    uint8_t message_opcode = 0;

    while (running_) {
      bool fin;
      uint8_t opcode;
      std::vector<uint8_t> payload;
      if (!readFrame(fin, opcode, payload)) break;

      // Control frames (opcodes >= 0x8) can appear between fragments
      if (opcode == 0x09) {
        // Ping — respond with pong
        std::lock_guard<std::mutex> lock(send_mutex_);
        sendFrame(0x0A, payload.data(), payload.size());
        continue;
      }

      if (opcode == 0x0A) {
        // Pong — ignore
        continue;
      }

      if (opcode == 0x08) {
        // Close frame
        if (payload.size() > 2) {
          close_reason.assign(payload.begin() + 2, payload.end());
        }
        // Echo close frame back
        {
          std::lock_guard<std::mutex> lock(send_mutex_);
          sendFrame(0x08, payload.data(), std::min(payload.size(), size_t(2)));
        }
        break;
      }

      // Data frames
      if (opcode != 0x00) {
        // New message (text=0x01 or binary=0x02)
        message_opcode = opcode;
        message_buffer = std::move(payload);
      } else {
        // Continuation frame
        message_buffer.insert(message_buffer.end(), payload.begin(),
                              payload.end());
      }

      if (fin && callback_) {
        Message msg;
        msg.type = MessageType::Message;
        msg.data.assign(message_buffer.begin(), message_buffer.end());
        msg.binary = (message_opcode == 0x02);
        callback_(msg);
        message_buffer.clear();
      }
    }
  }

  void fireError(const std::string &reason) {
    if (callback_) {
      callback_({.type = MessageType::Error, .error_reason = reason});
    }
  }

  void sleepMs(uint32_t ms) {
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (running_ && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
};

}  // namespace ws
