#include "WL_Socket.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
static double now_seconds() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

static bool wait_readable(int fd, int timeout_ms) {
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(fd, &rfds);

  struct timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  return ::select(fd + 1, &rfds, nullptr, nullptr, &tv) > 0;
}

static bool send_all(int fd, const std::string &msg) {
  std::size_t sent = 0;
  while (sent < msg.size()) {
    ssize_t n = ::send(fd, msg.data() + sent, msg.size() - sent, 0);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

static bool recv_all(int fd, std::string &msg, int timeout_ms) {
  if (!wait_readable(fd, timeout_ms)) {
    return false;
  }

  char buf[4096];
  ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
  if (n <= 0) {
    return false;
  }

  msg.assign(buf, static_cast<std::size_t>(n));
  return true;
}

static bool send_frame(int fd, const std::string &msg) {
  uint32_t len = static_cast<uint32_t>(msg.size());
  len = htonl(len);

  std::string frame;
  frame.resize(sizeof(len) + msg.size());
  std::memcpy(frame.data(), &len, sizeof(len));
  std::memcpy(frame.data() + sizeof(len), msg.data(), msg.size());

  return send_all(fd, frame);
}

static bool recv_frame(int fd, std::string &msg, int timeout_ms) {
  if (!wait_readable(fd, timeout_ms)) {
    return false;
  }

  uint32_t len_net = 0;
  ssize_t n = ::recv(fd, &len_net, sizeof(len_net), MSG_WAITALL);
  if (n != static_cast<ssize_t>(sizeof(len_net))) {
    return false;
  }

  uint32_t len = ntohl(len_net);
  std::vector<char> buf(len);

  std::size_t off = 0;
  while (off < len) {
    ssize_t r = ::recv(fd, buf.data() + off, len - off, 0);
    if (r <= 0) {
      return false;
    }
    off += static_cast<std::size_t>(r);
  }

  msg.assign(buf.data(), len);
  return true;
}

static bool recv_udp(int fd, std::string &msg, sockaddr_in &from,
                     socklen_t &from_len, int timeout_ms) {
  if (!wait_readable(fd, timeout_ms)) {
    return false;
  }

  char buf[65535];
  ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0,
                         reinterpret_cast<sockaddr *>(&from), &from_len);
  if (n <= 0) {
    return false;
  }

  msg.assign(buf, static_cast<std::size_t>(n));
  return true;
}

static bool send_udp(int fd, const sockaddr_in &to, const std::string &msg) {
  ssize_t n = ::sendto(fd, msg.data(), msg.size(), 0,
                       reinterpret_cast<const sockaddr *>(&to), sizeof(to));
  return n == static_cast<ssize_t>(msg.size());
}

} // namespace

WL_Socket::WL_Socket(Type type, Role role, int socketOption)
    : type_(type), role_(role), socketOption_(socketOption), socket_(-1) {}

WL_Socket::~WL_Socket() { Stop(); }

void WL_Socket::Start() {
  if (socket_ >= 0) {
    return;
  }

  int protocol = (type_ == UDP) ? IPPROTO_UDP : IPPROTO_TCP;
  int domain = AF_INET;
  int socktype = (type_ == UDP) ? SOCK_DGRAM : SOCK_STREAM;

  socket_ = ::socket(domain, socktype, protocol);
  if (socket_ < 0) {
    std::cerr << "socket() failed: " << std::strerror(errno) << std::endl;
    socket_ = -1;
    return;
  }

  int opt = 1;
  if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    std::cerr << "setsockopt() failed: " << std::strerror(errno) << std::endl;
  }
}

void WL_Socket::Stop() {
  if (socket_ >= 0) {
    ::close(socket_);
    socket_ = -1;
  }
}

bool WL_Socket::Send(const std::string &data) {
  if (socket_ < 0 || data.empty()) {
    return false;
  }

  if (type_ == UDP) {
    return ::send(socket_, data.data(), data.size(), 0) ==
           static_cast<ssize_t>(data.size());
  }

  ssize_t sent = 0;
  while (sent < static_cast<ssize_t>(data.size())) {
    ssize_t n = ::send(socket_, data.data() + sent, data.size() - sent, 0);
    if (n <= 0) {
      return false;
    }
    sent += n;
  }
  return true;
}

bool WL_Socket::Receive(std::string &data) {
  if (socket_ < 0) {
    return false;
  }

  char buffer[4096];
  ssize_t n = ::recv(socket_, buffer, sizeof(buffer), 0);
  if (n <= 0) {
    return false;
  }

  data.assign(buffer, static_cast<std::size_t>(n));
  return true;
}

double WL_Socket::client_bandwidth_test(const std::string &server_ip, int port,
                                        std::size_t duration_seconds,
                                        std::size_t payload_size) {
  if (payload_size == 0 || duration_seconds == 0) {
    return 0.0;
  }

  const std::size_t safe_payload_size =
      (type_ == UDP) ? std::min(payload_size, static_cast<std::size_t>(1400u))
                     : payload_size;

  std::string payload(safe_payload_size, 'A');

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) != 1) {
    std::cerr << "Invalid server IP: " << server_ip << std::endl;
    return 0.0;
  }

  int fd = socket(AF_INET, (type_ == UDP) ? SOCK_DGRAM : SOCK_STREAM,
                  (type_ == UDP) ? IPPROTO_UDP : IPPROTO_TCP);
  if (fd < 0) {
    std::cerr << "socket() failed: " << std::strerror(errno) << std::endl;
    return 0.0;
  }

  std::string msg;

  if (type_ == UDP) {
    bool started = false;
    const double handshake_deadline = now_seconds() + 5.0;

    while (now_seconds() < handshake_deadline) {
      if (send_udp(fd, server_addr, "START")) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        if (recv_udp(fd, msg, from, from_len, 200) && msg == "OK") {
          started = true;
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!started) {
      std::cerr << "UDP client: START handshake failed" << std::endl;
      ::close(fd);
      return 0.0;
    }
  } else {
    if (::connect(fd, reinterpret_cast<sockaddr *>(&server_addr),
                  sizeof(server_addr)) < 0) {
      std::cerr << "TCP client: connect() failed: " << std::strerror(errno)
                << std::endl;
      ::close(fd);
      return 0.0;
    }

    bool started = false;
    const double handshake_deadline = now_seconds() + 5.0;

    while (now_seconds() < handshake_deadline) {
      if (send_frame(fd, "START")) {
        std::string reply;
        if (recv_frame(fd, reply, 200) && reply == "OK") {
          started = true;
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!started) {
      std::cerr << "TCP client: START handshake failed" << std::endl;
      ::close(fd);
      return 0.0;
    }
  }

  const double start = now_seconds();
  const double deadline = start + static_cast<double>(duration_seconds);

  std::size_t total_sent = 0;

  while (now_seconds() < deadline) {
    if (type_ == UDP) {
      ssize_t n = ::sendto(fd, payload.data(), payload.size(), 0,
                           reinterpret_cast<const sockaddr *>(&server_addr),
                           sizeof(server_addr));
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        std::cerr << "send() failed: " << std::strerror(errno) << std::endl;
        break;
      }
      total_sent += static_cast<std::size_t>(n);
    } else {
      if (!send_frame(fd, payload)) {
        std::cerr << "TCP client: send payload failed" << std::endl;
        break;
      }
      total_sent += payload.size();
    }
  }

  if (type_ == UDP) {
    bool stopped = false;
    const double stop_deadline = now_seconds() + 5.0;

    while (now_seconds() < stop_deadline) {
      if (send_udp(fd, server_addr, "STOP")) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        if (recv_udp(fd, msg, from, from_len, 200) && msg == "OK") {
          stopped = true;
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!stopped) {
      std::cerr << "UDP client: STOP handshake failed" << std::endl;
      ::close(fd);
      return 0.0;
    }
  } else {
    bool stopped = false;
    const double stop_deadline = now_seconds() + 5.0;

    while (now_seconds() < stop_deadline) {
      if (send_frame(fd, "STOP")) {
        std::string reply;
        if (recv_frame(fd, reply, 200) && reply == "OK") {
          stopped = true;
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!stopped) {
      std::cerr << "TCP client: STOP handshake failed" << std::endl;
      ::close(fd);
      return 0.0;
    }
  }

  ::close(fd);

  const double elapsed = std::max(0.0, now_seconds() - start);
  const double bytes_per_second =
      (elapsed > 0.0) ? (static_cast<double>(total_sent) / elapsed) : 0.0;

  return bytes_per_second * 8.0 / 1000000.0; // Mb/s
}

double WL_Socket::server_bandwidth_test(int port, std::size_t duration_seconds,
                                        std::size_t buffer_size) {
  if (buffer_size == 0 || duration_seconds == 0) {
    return 0.0;
  }

  int fd = socket(AF_INET, (type_ == UDP) ? SOCK_DGRAM : SOCK_STREAM,
                  (type_ == UDP) ? IPPROTO_UDP : IPPROTO_TCP);
  if (fd < 0) {
    std::cerr << "socket() failed: " << std::strerror(errno) << std::endl;
    return 0.0;
  }

  int opt = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    std::cerr << "setsockopt() failed: " << std::strerror(errno) << std::endl;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    std::cerr << "bind() failed on port " << port << ": "
              << std::strerror(errno) << std::endl;
    ::close(fd);
    return 0.0;
  }

  std::size_t total_received = 0;
  std::vector<char> buffer(buffer_size);

  const double start = now_seconds();
  const double deadline = start + static_cast<double>(duration_seconds);

  if (type_ == UDP) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    std::string msg;

    const double handshake_deadline = now_seconds() + 5.0;
    bool started = false;

    while (now_seconds() < handshake_deadline) {
      char recv_buf[4096];
      ssize_t n =
          ::recvfrom(fd, recv_buf, sizeof(recv_buf), 0,
                     reinterpret_cast<sockaddr *>(&client_addr), &client_len);
      if (n > 0) {
        msg.assign(recv_buf, static_cast<std::size_t>(n));
        if (msg == "START") {
          if (!send_udp(fd, client_addr, "OK")) {
            std::cerr << "UDP server: cannot send OK" << std::endl;
            ::close(fd);
            return 0.0;
          }
          started = true;
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!started) {
      std::cerr << "UDP server: START handshake failed" << std::endl;
      ::close(fd);
      return 0.0;
    }

    const double stop_deadline = deadline + 5.0;

    while (now_seconds() < stop_deadline) {
      sockaddr_in from{};
      socklen_t from_len = sizeof(from);
      char pkt[65535];
      ssize_t n = ::recvfrom(fd, pkt, sizeof(pkt), 0,
                             reinterpret_cast<sockaddr *>(&from), &from_len);
      if (n > 0) {
        std::string packet(pkt, static_cast<std::size_t>(n));
        if (packet == "STOP") {
          if (!send_udp(fd, from, "OK")) {
            std::cerr << "UDP server: STOP reply failed" << std::endl;
          }
          break;
        }

        if (now_seconds() < deadline) {
          total_received += static_cast<std::size_t>(n);
        }
      }
    }
  } else {
    if (::listen(fd, 1) < 0) {
      std::cerr << "listen() failed: " << std::strerror(errno) << std::endl;
      ::close(fd);
      return 0.0;
    }

    int client_fd = ::accept(fd, nullptr, nullptr);
    if (client_fd < 0) {
      std::cerr << "accept() failed: " << std::strerror(errno) << std::endl;
      ::close(fd);
      return 0.0;
    }

    std::string msg;
    const double handshake_deadline = now_seconds() + 5.0;
    bool started = false;

    while (now_seconds() < handshake_deadline) {
      if (recv_frame(client_fd, msg, 200) && msg == "START") {
        if (!send_frame(client_fd, "OK")) {
          std::cerr << "TCP server: cannot send OK" << std::endl;
          ::close(client_fd);
          ::close(fd);
          return 0.0;
        }
        started = true;
        break;
      }
    }

    if (!started) {
      std::cerr << "TCP server: START handshake failed" << std::endl;
      ::close(client_fd);
      ::close(fd);
      return 0.0;
    }

    const double stop_deadline = deadline + 5.0;

    while (now_seconds() < stop_deadline) {
      std::string pkt;
      if (!recv_frame(client_fd, pkt, 200)) {
        break;
      }

      if (pkt == "STOP") {
        if (!send_frame(client_fd, "OK")) {
          std::cerr << "TCP server: STOP reply failed" << std::endl;
        }
        break;
      }

      if (now_seconds() < deadline) {
        total_received += pkt.size();
      }
    }

    ::close(client_fd);
  }

  ::close(fd);

  const double elapsed = std::max(0.0, now_seconds() - start);
  const double bytes_per_second =
      (elapsed > 0.0) ? (static_cast<double>(total_received) / elapsed) : 0.0;

  return bytes_per_second * 8.0 / 1000000.0; // Mb/s
}
