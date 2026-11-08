#include "WL_Socket.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
static double now_seconds() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}
} // namespace

WL_Socket::WL_Socket(Type type, Role role, int socketOption)
    : type_(type), role_(role), socketOption_(socketOption), socket_(-1) {
  socket_ = socket(AF_INET, (type_ == TCP ? SOCK_STREAM : SOCK_DGRAM), 0);
  if (socket_ == -1) {
    std::cerr << "Error creating socket: " << std::strerror(errno) << std::endl;
    return;
  }

  if (socketOption_ != 0) {
    int opt = 1;
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) ==
        -1) {
      std::cerr << "Error setting socket option: " << std::strerror(errno)
                << std::endl;
    }
  }
}

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
    socket_ = -1;
    return;
  }

  int opt = 1;
  setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
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

  if (::connect(fd, reinterpret_cast<sockaddr *>(&server_addr),
                sizeof(server_addr)) < 0) {
    std::cerr << "connect() failed to " << server_ip << ":" << port << " -> "
              << std::strerror(errno) << std::endl;
    ::close(fd);
    return 0.0;
  }

  const double start = now_seconds();
  const double deadline = start + static_cast<double>(duration_seconds);

  std::size_t total_sent = 0;

  while (now_seconds() < deadline) {
    const ssize_t n = ::send(fd, payload.data(), payload.size(), 0);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      if (errno == ECONNREFUSED) {
        std::cerr << "UDP server not listening on " << server_ip << ":" << port
                  << std::endl;
      } else {
        std::cerr << "send() failed: " << std::strerror(errno) << std::endl;
      }
      break;
    }
    total_sent += static_cast<std::size_t>(n);
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
    while (now_seconds() < deadline) {
      sockaddr_in from{};
      socklen_t from_len = sizeof(from);

      ssize_t n = ::recvfrom(fd, buffer.data(), buffer.size(), 0,
                             reinterpret_cast<sockaddr *>(&from), &from_len);
      if (n > 0) {
        total_received += static_cast<std::size_t>(n);
      } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        std::cerr << "recvfrom() failed: " << std::strerror(errno) << std::endl;
        break;
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

    while (now_seconds() < deadline) {
      ssize_t n = ::recv(client_fd, buffer.data(), buffer.size(), 0);
      if (n > 0) {
        total_received += static_cast<std::size_t>(n);
      } else if (n <= 0) {
        break;
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
