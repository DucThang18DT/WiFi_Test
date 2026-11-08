#include "WL_Socket.h"

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
  if (role_ == SERVER) {
    struct sockaddr_in serverAddress;
    std::memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(8080);

    if (bind(socket_, reinterpret_cast<struct sockaddr *>(&serverAddress),
             sizeof(serverAddress)) == -1) {
      std::cerr << "Error binding socket: " << std::strerror(errno)
                << std::endl;
      return;
    }

    if (type_ == TCP) {
      if (listen(socket_, 5) == -1) {
        std::cerr << "Error listening: " << std::strerror(errno) << std::endl;
        return;
      }

      std::cout << "TCP server listening on port 8080." << std::endl;

      while (true) {
        struct sockaddr_in clientAddress;
        std::memset(&clientAddress, 0, sizeof(clientAddress));
        socklen_t clientSize = sizeof(clientAddress);

        int clientSocket =
            accept(socket_, reinterpret_cast<struct sockaddr *>(&clientAddress),
                   &clientSize);

        if (clientSocket == -1) {
          std::cerr << "Error accepting connection: " << std::strerror(errno)
                    << std::endl;
          continue;
        }

        char buffer[1024];
        std::memset(buffer, 0, sizeof(buffer));

        ssize_t bytesRead = read(clientSocket, buffer, sizeof(buffer) - 1);
        if (bytesRead > 0) {
          buffer[bytesRead] = '\0';
          std::cout << "Received TCP data: " << buffer << std::endl;
        }

        close(clientSocket);
      }
    }
  } else if (role_ == CLIENT) {
    struct sockaddr_in serverAddress;
    std::memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(8080);

    if (inet_pton(AF_INET, "127.0.0.1", &serverAddress.sin_addr) != 1) {
      std::cerr << "Invalid server IP address." << std::endl;
      return;
    }

    if (connect(socket_, reinterpret_cast<struct sockaddr *>(&serverAddress),
                sizeof(serverAddress)) == -1) {
      std::cerr << "Error connecting to server: " << std::strerror(errno)
                << std::endl;
      return;
    }

    std::cout << "Connected to server." << std::endl;

    if (type_ == TCP) {
      const std::string message = "Hello, Server!";
      if (send(socket_, message.c_str(), message.size(), 0) == -1) {
        std::cerr << "Error sending data: " << std::strerror(errno)
                  << std::endl;
      }
    }
  }
}

void WL_Socket::Stop() {
  if (socket_ != -1) {
    close(socket_);
    socket_ = -1;
  }
}

bool WL_Socket::Send(const std::string &data) {
  if (socket_ == -1) {
    return false;
  }

  if (type_ == TCP) {
    ssize_t sent = send(socket_, data.c_str(), data.size(), 0);
    return sent != -1;
  }

  return false;
}

bool WL_Socket::Receive(std::string &data) {
  if (socket_ == -1) {
    return false;
  }

  if (type_ == TCP) {
    char buffer[1024];
    std::memset(buffer, 0, sizeof(buffer));

    ssize_t bytesRead = read(socket_, buffer, sizeof(buffer) - 1);
    if (bytesRead > 0) {
      buffer[bytesRead] = '\0';
      data = buffer;
      return true;
    }
  }

  return false;
}

double WL_Socket::client_bandwidth_test(const std::string &server_ip, int port,
                                        std::size_t duration_seconds,
                                        std::size_t payload_size) {
  if (payload_size == 0 || duration_seconds == 0) {
    return 0.0;
  }

  std::string payload(payload_size, 'A');

  struct sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) != 1) {
    return 0.0;
  }

  int fd = socket(AF_INET, (type_ == UDP) ? SOCK_DGRAM : SOCK_STREAM,
                  (type_ == UDP) ? IPPROTO_UDP : IPPROTO_TCP);
  if (fd < 0) {
    return 0.0;
  }

  if (type_ == TCP) {
    if (::connect(fd, reinterpret_cast<sockaddr *>(&server_addr),
                  sizeof(server_addr)) < 0) {
      ::close(fd);
      return 0.0;
    }
  } else {
    if (::connect(fd, reinterpret_cast<sockaddr *>(&server_addr),
                  sizeof(server_addr)) < 0) {
      ::close(fd);
      return 0.0;
    }
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
      break;
    }
    total_sent += static_cast<std::size_t>(n);
  }

  ::close(fd);

  const double elapsed = std::max(0.0, now_seconds() - start);
  return (elapsed > 0.0) ? (static_cast<double>(total_sent) / elapsed) : 0.0;
}

double WL_Socket::server_bandwidth_test(int port, std::size_t duration_seconds,
                                        std::size_t buffer_size) {
  if (buffer_size == 0 || duration_seconds == 0) {
    return 0.0;
  }

  int fd = socket(AF_INET, (type_ == UDP) ? SOCK_DGRAM : SOCK_STREAM,
                  (type_ == UDP) ? IPPROTO_UDP : IPPROTO_TCP);
  if (fd < 0) {
    return 0.0;
  }

  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
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
        break;
      }
    }
  } else {
    if (::listen(fd, 1) < 0) {
      ::close(fd);
      return 0.0;
    }

    int client_fd = ::accept(fd, nullptr, nullptr);
    if (client_fd < 0) {
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
  return (elapsed > 0.0) ? (static_cast<double>(total_received) / elapsed)
                         : 0.0;
}
