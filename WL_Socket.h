#ifndef WL_SOCKET_H
#define WL_SOCKET_H

#include <cstddef>
#include <string>

class WL_Socket {
public:
  enum Type { TCP, UDP };
  enum Role { SERVER, CLIENT };

  WL_Socket(Type type = TCP, Role role = CLIENT, int socketOption = 0);
  ~WL_Socket();

  void Start();
  void Stop();

  bool Send(const std::string &data);
  bool Receive(std::string &data);

  double client_bandwidth_test(const std::string &server_ip = "127.0.0.1",
                               int port = 8080,
                               std::size_t duration_seconds = 5,
                               std::size_t payload_size = 1024 * 1024);

  double server_bandwidth_test(int port = 8080,
                               std::size_t duration_seconds = 5,
                               std::size_t buffer_size = 64 * 1024);

private:
  Type type_;
  Role role_;
  int socketOption_;
  int socket_;
};

#endif // WL_SOCKET_H
