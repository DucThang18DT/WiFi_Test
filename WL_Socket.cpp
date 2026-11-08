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
  if (type_ != TCP || role_ != CLIENT) {
    std::cerr << "client_bandwidth_test requires TCP client socket."
              << std::endl;
    return -1.0;
  }

  sockaddr_in serverAddress{};
  std::memset(&serverAddress, 0, sizeof(serverAddress));
  serverAddress.sin_family = AF_INET;
  serverAddress.sin_port = htons(port);

  if (inet_pton(AF_INET, server_ip.c_str(), &serverAddress.sin_addr) != 1) {
    std::cerr << "Invalid server IP address." << std::endl;
    return -1.0;
  }

  if (connect(socket_, reinterpret_cast<sockaddr *>(&serverAddress),
              sizeof(serverAddress)) == -1) {
    std::cerr << "Error connecting to server: " << std::strerror(errno)
              << std::endl;
    return -1.0;
  }

  std::vector<char> payload(payload_size, 'A');
  auto start = std::chrono::steady_clock::now();
  long long total = 0;

  while (true) {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start).count();

    if (elapsed >= static_cast<double>(duration_seconds)) {
      break;
    }

    ssize_t sent = send(socket_, payload.data(), payload.size(), 0);
    if (sent <= 0) {
      break;
    }

    total += sent;
  }

  char ack[32];
  std::memset(ack, 0, sizeof(ack));
  recv(socket_, ack, sizeof(ack) - 1, 0);

  auto end = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(end - start).count();

  double throughput = elapsed > 0.0 ? total / (1024.0 * 1024.0 * elapsed) : 0.0;

  std::cout << "Client sent: " << total << " bytes" << std::endl;
  std::cout << "Client throughput: " << throughput << " MB/s" << std::endl;

  return throughput;
}

double WL_Socket::server_bandwidth_test(int port, std::size_t duration_seconds,
                                        std::size_t buffer_size) {
  if (type_ != TCP || role_ != SERVER) {
    std::cerr << "server_bandwidth_test requires TCP server socket."
              << std::endl;
    return -1.0;
  }

  sockaddr_in serverAddress{};
  std::memset(&serverAddress, 0, sizeof(serverAddress));
  serverAddress.sin_family = AF_INET;
  serverAddress.sin_addr.s_addr = INADDR_ANY;
  serverAddress.sin_port = htons(port);

  if (bind(socket_, reinterpret_cast<sockaddr *>(&serverAddress),
           sizeof(serverAddress)) == -1) {
    std::cerr << "Error binding socket: " << std::strerror(errno) << std::endl;
    return -1.0;
  }

  if (listen(socket_, 5) == -1) {
    std::cerr << "Error listening: " << std::strerror(errno) << std::endl;
    return -1.0;
  }

  sockaddr_in clientAddress{};
  socklen_t clientSize = sizeof(clientAddress);

  int clientSocket = accept(
      socket_, reinterpret_cast<sockaddr *>(&clientAddress), &clientSize);
  if (clientSocket == -1) {
    std::cerr << "Error accepting connection: " << std::strerror(errno)
              << std::endl;
    return -1.0;
  }

  std::vector<char> buffer(buffer_size, 0);
  auto start = std::chrono::steady_clock::now();
  long long total = 0;

  while (true) {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start).count();

    if (elapsed >= static_cast<double>(duration_seconds)) {
      break;
    }

    ssize_t received = recv(clientSocket, buffer.data(), buffer.size(), 0);
    if (received <= 0) {
      break;
    }

    total += received;
  }

  auto end = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(end - start).count();

  const std::string ok = "OK";
  send(clientSocket, ok.c_str(), ok.size(), 0);

  close(clientSocket);

  double throughput = elapsed > 0.0 ? total / (1024.0 * 1024.0 * elapsed) : 0.0;

  std::cout << "Server received: " << total << " bytes" << std::endl;
  std::cout << "Server throughput: " << throughput << " MB/s" << std::endl;

  return throughput;
}
