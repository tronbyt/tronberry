// httpclient.h — Minimal HTTP/HTTPS client. Header-only, no exceptions.
// Uses POSIX sockets + OpenSSL for TLS. Supports Content-Length and
// chunked transfer encoding. One connection per request (Connection: close).

#pragma once

#include <openssl/ssl.h>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace http {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Response {
  int status = 0;
  std::string body;
  Headers headers;

  std::string get_header_value(const std::string &name,
                               const std::string &default_val = "") const {
    std::string lower;
    lower.resize(name.size());
    std::transform(name.begin(), name.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (auto &[k, v] : headers) {
      if (k == lower) return v;
    }
    return default_val;
  }
};

class Client {
 public:
  explicit Client(const std::string &base_url) {
    size_t pos = base_url.find("://");
    if (pos == std::string::npos) return;
    std::string scheme = base_url.substr(0, pos);
    use_tls_ = (scheme == "https");
    if (scheme != "http" && scheme != "https") return;

    std::string host_port = base_url.substr(pos + 3);
    size_t slash = host_port.find('/');
    if (slash != std::string::npos) host_port = host_port.substr(0, slash);

    size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
      host_ = host_port.substr(0, colon);
      port_ = std::stoi(host_port.substr(colon + 1));
    } else {
      host_ = host_port;
      port_ = use_tls_ ? 443 : 80;
    }
    valid_ = !host_.empty();
  }

  bool is_valid() const { return valid_; }

  void set_default_headers(Headers headers) {
    default_headers_ = std::move(headers);
  }

  void set_connection_timeout(int seconds) { connect_timeout_sec_ = seconds; }
  void set_read_timeout(int seconds) { read_timeout_sec_ = seconds; }

  std::unique_ptr<Response> Get(const char *path) {
    int sockfd = tcpConnect();
    if (sockfd < 0) return nullptr;

    SSL_CTX *ssl_ctx = nullptr;
    SSL *ssl = nullptr;

    if (use_tls_) {
      ssl_ctx = SSL_CTX_new(TLS_client_method());
      if (!ssl_ctx) {
        ::close(sockfd);
        return nullptr;
      }
      ssl = SSL_new(ssl_ctx);
      if (!ssl) {
        SSL_CTX_free(ssl_ctx);
        ::close(sockfd);
        return nullptr;
      }
      SSL_set_fd(ssl, sockfd);
      SSL_set_tlsext_host_name(ssl, host_.c_str());
      if (SSL_connect(ssl) != 1) {
        cleanup(sockfd, ssl, ssl_ctx);
        return nullptr;
      }
    }

    // Build request
    std::string req = "GET " + std::string(path) +
                      " HTTP/1.1\r\n"
                      "Host: " +
                      host_ + "\r\nConnection: close\r\n";
    for (auto &[k, v] : default_headers_) {
      req += k + ": " + v + "\r\n";
    }
    req += "\r\n";

    if (!rawSend(sockfd, ssl, req.data(), req.size())) {
      cleanup(sockfd, ssl, ssl_ctx);
      return nullptr;
    }

    // Read response headers (byte at a time until \r\n\r\n)
    std::string hdr;
    char c;
    while (rawRecv(sockfd, ssl, &c, 1)) {
      hdr += c;
      if (hdr.size() >= 4 &&
          hdr.compare(hdr.size() - 4, 4, "\r\n\r\n") == 0) {
        break;
      }
      if (hdr.size() > 16384) {
        cleanup(sockfd, ssl, ssl_ctx);
        return nullptr;
      }
    }

    if (hdr.size() < 12) {
      cleanup(sockfd, ssl, ssl_ctx);
      return nullptr;
    }

    // Parse status line: HTTP/1.x NNN ...
    auto res = std::make_unique<Response>();
    size_t sp = hdr.find(' ');
    if (sp == std::string::npos) {
      cleanup(sockfd, ssl, ssl_ctx);
      return nullptr;
    }
    res->status = std::atoi(hdr.c_str() + sp + 1);

    // Parse headers
    size_t pos = hdr.find("\r\n");
    if (pos != std::string::npos) pos += 2;
    while (pos < hdr.size()) {
      size_t eol = hdr.find("\r\n", pos);
      if (eol == std::string::npos || eol == pos) break;
      size_t colon = hdr.find(':', pos);
      if (colon != std::string::npos && colon < eol) {
        std::string key = hdr.substr(pos, colon - pos);
        std::string val = hdr.substr(colon + 1, eol - colon - 1);
        // Trim leading whitespace from value
        size_t vs = val.find_first_not_of(" \t");
        if (vs != std::string::npos) val = val.substr(vs);
        // Lowercase key for case-insensitive lookup
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char ch) { return std::tolower(ch); });
        res->headers.emplace_back(std::move(key), std::move(val));
      }
      pos = eol + 2;
    }

    // Read body
    std::string te = res->get_header_value("transfer-encoding");
    std::string cl = res->get_header_value("content-length");

    if (te.find("chunked") != std::string::npos) {
      if (!readChunkedBody(sockfd, ssl, res->body)) {
        cleanup(sockfd, ssl, ssl_ctx);
        return nullptr;
      }
    } else if (!cl.empty()) {
      size_t len = std::strtoul(cl.c_str(), nullptr, 10);
      res->body.resize(len);
      if (len > 0 && !rawRecv(sockfd, ssl, res->body.data(), len)) {
        cleanup(sockfd, ssl, ssl_ctx);
        return nullptr;
      }
    } else {
      readUntilClose(sockfd, ssl, res->body);
    }

    cleanup(sockfd, ssl, ssl_ctx);
    return res;
  }

 private:
  std::string host_;
  int port_ = 0;
  bool use_tls_ = false;
  bool valid_ = false;
  int connect_timeout_sec_ = 30;
  int read_timeout_sec_ = 30;
  Headers default_headers_;

  int tcpConnect() {
    struct addrinfo hints {}, *result;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result) != 0)
      return -1;

    int sockfd = -1;
    for (auto *rp = result; rp; rp = rp->ai_next) {
      sockfd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (sockfd < 0) continue;

      // Non-blocking connect with timeout
      int flags = fcntl(sockfd, F_GETFL, 0);
      fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

      int rc = ::connect(sockfd, rp->ai_addr, rp->ai_addrlen);
      if (rc == 0) {
        // Connected immediately
      } else if (errno == EINPROGRESS) {
        struct pollfd pfd = {sockfd, POLLOUT, 0};
        if (poll(&pfd, 1, connect_timeout_sec_ * 1000) <= 0) {
          ::close(sockfd);
          sockfd = -1;
          continue;
        }
        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
          ::close(sockfd);
          sockfd = -1;
          continue;
        }
      } else {
        ::close(sockfd);
        sockfd = -1;
        continue;
      }

      // Restore blocking mode and set read timeout
      fcntl(sockfd, F_SETFL, flags);
      struct timeval tv;
      tv.tv_sec = read_timeout_sec_;
      tv.tv_usec = 0;
      setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      freeaddrinfo(result);
      return sockfd;
    }

    freeaddrinfo(result);
    return -1;
  }

  static void cleanup(int sockfd, SSL *ssl, SSL_CTX *ssl_ctx) {
    if (ssl) {
      SSL_shutdown(ssl);
      SSL_free(ssl);
    }
    if (ssl_ctx) SSL_CTX_free(ssl_ctx);
    if (sockfd >= 0) ::close(sockfd);
  }

  static bool rawSend(int sockfd, SSL *ssl, const void *data, size_t len) {
    auto *ptr = static_cast<const uint8_t *>(data);
    while (len > 0) {
      ssize_t n;
      if (ssl) {
        n = SSL_write(ssl, ptr, static_cast<int>(len));
      } else {
        n = ::send(sockfd, ptr, len, MSG_NOSIGNAL);
      }
      if (n <= 0) return false;
      ptr += n;
      len -= static_cast<size_t>(n);
    }
    return true;
  }

  static bool rawRecv(int sockfd, SSL *ssl, void *data, size_t len) {
    auto *ptr = static_cast<uint8_t *>(data);
    while (len > 0) {
      ssize_t n;
      if (ssl) {
        n = SSL_read(ssl, ptr, static_cast<int>(len));
      } else {
        n = ::recv(sockfd, ptr, len, 0);
      }
      if (n <= 0) return false;
      ptr += n;
      len -= static_cast<size_t>(n);
    }
    return true;
  }

  // Read a line terminated by \r\n, return without the terminator.
  static bool readLine(int sockfd, SSL *ssl, std::string &line) {
    line.clear();
    char c;
    while (rawRecv(sockfd, ssl, &c, 1)) {
      if (c == '\r') {
        rawRecv(sockfd, ssl, &c, 1);  // consume \n
        return true;
      }
      line += c;
      if (line.size() > 16384) return false;
    }
    return false;
  }

  static bool readChunkedBody(int sockfd, SSL *ssl, std::string &body) {
    std::string size_line;
    while (true) {
      if (!readLine(sockfd, ssl, size_line)) return false;
      size_t chunk_size = std::strtoul(size_line.c_str(), nullptr, 16);
      if (chunk_size == 0) break;

      size_t offset = body.size();
      body.resize(offset + chunk_size);
      if (!rawRecv(sockfd, ssl, body.data() + offset, chunk_size)) return false;

      // Consume trailing \r\n after chunk data
      char crlf[2];
      if (!rawRecv(sockfd, ssl, crlf, 2)) return false;

      size_line.clear();
    }
    // Consume optional trailers + final \r\n
    while (true) {
      if (!readLine(sockfd, ssl, size_line)) break;
      if (size_line.empty()) break;
      size_line.clear();
    }
    return true;
  }

  static void readUntilClose(int sockfd, SSL *ssl, std::string &body) {
    char buf[8192];
    while (true) {
      ssize_t n;
      if (ssl) {
        n = SSL_read(ssl, buf, sizeof(buf));
      } else {
        n = ::recv(sockfd, buf, sizeof(buf), 0);
      }
      if (n <= 0) break;
      body.append(buf, static_cast<size_t>(n));
    }
  }
};

}  // namespace http
