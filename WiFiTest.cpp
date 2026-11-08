#include "WL_Socket.h"

#include <atomic>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

static std::atomic<bool> g_stopRequested{false};
static WL_Socket *g_socket = nullptr;

static std::string trim(const std::string &s) {
  std::size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }

  std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

static std::map<std::string, std::string>
readConfig(const std::string &filename) {
  std::ifstream file(filename);
  std::map<std::string, std::string> config;

  if (!file) {
    std::cerr << "Cannot open config: " << filename << std::endl;
    return config;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::size_t pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }

    std::string key = trim(line.substr(0, pos));
    std::string value = trim(line.substr(pos + 1));

    if (!key.empty()) {
      config[key] = value;
    }
  }

  return config;
}

static void handleSignal(int signal) {
  std::cout << "\nReceived signal: " << signal << std::endl;
  g_stopRequested.store(true);

  if (g_socket != nullptr) {
    g_socket->Stop();
  }

  std::exit(0);
}

static void installSignalHandlers() {
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handleSignal;
  sigemptyset(&sa.sa_mask);

  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGQUIT, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);
}

int main(int argc, char *argv[]) {
  std::string configPath = (argc > 1) ? argv[1] : "socket.conf";
  auto cfg = readConfig(configPath);

  if (cfg.empty()) {
    std::cerr << "Config file is empty or invalid: " << configPath << std::endl;
    return 1;
  }

  std::string roleStr = cfg.count("Role") ? cfg["Role"] : "Server";
  std::string typeStr = cfg.count("Type") ? cfg["Type"] : "TCP";
  std::string ip = cfg.count("ServerAddr") ? cfg["ServerAddr"] : "127.0.0.1";
  int port = cfg.count("Port") ? std::stoi(cfg["Port"]) : 8080;

  WL_Socket::Type type = (typeStr == "TCP" || typeStr == "tcp")
                             ? WL_Socket::Type::TCP
                             : WL_Socket::Type::UDP;

  WL_Socket::Role role =
      (roleStr == "Server" || roleStr == "SERVER" || roleStr == "server")
          ? WL_Socket::Role::SERVER
          : WL_Socket::Role::CLIENT;

  installSignalHandlers();

  WL_Socket socket(type, role, 1);
  g_socket = &socket;

  if (type == WL_Socket::Type::TCP) {
    if (role == WL_Socket::Role::SERVER) {
      socket.server_bandwidth_test(port, 5, 64 * 1024);
    } else {
      socket.client_bandwidth_test(ip, port, 5, 1024 * 1024);
    }
  } else {
    std::cerr << "Only TCP is supported for bandwidth test." << std::endl;
    return 1;
  }

  return 0;
}
