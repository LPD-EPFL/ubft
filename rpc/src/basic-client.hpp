#pragma once

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>   //inet_addr
#include <sys/socket.h>  //socket
#include <unistd.h>

#include <dory/shared/move-indicator.hpp>

namespace dory::rpc {
class RpcBasicClient {
 public:
  RpcBasicClient(RpcBasicClient const &) = delete;
  RpcBasicClient &operator=(RpcBasicClient const &) = delete;
  RpcBasicClient(RpcBasicClient &&) = default;
  RpcBasicClient &operator=(RpcBasicClient &&) = default;

  RpcBasicClient(std::string ip, int port)
      : ip{std::move(ip)}, port{port}, sock{-1} {}

  bool connect() {
    if (sock != -1) {
      return false;
    }

    std::memset(&server, 0, sizeof(server));

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
      return false;
    }

    server.sin_addr.s_addr = inet_addr(ip.c_str());
    server.sin_family = AF_INET;
    server.sin_port = htons(static_cast<uint16_t>(port));

    if (::connect(sock, reinterpret_cast<struct sockaddr *>(&server),
                  sizeof(server)) < 0) {
      perror("connect failed. Error");
      return false;
    }

    return true;
  }

  bool send(void const *buf, size_t len) const {
    auto ret = ::send(sock, buf, len, 0);

    if (ret < 0) {
      perror("Send failed : ");
      return false;
    }

    return !(ret >= 0 && ret != static_cast<ssize_t>(len));
  }

  std::vector<char> recv(size_t len = 512) const {
    std::vector<char> buf(len);
    buf.reserve(len);

    auto ret = ::recv(sock, buf.data(), len, 0);
    if (ret <= 0) {
      buf.clear();
      return buf;
    }

    buf.resize(static_cast<size_t>(ret));
    return buf;
  }

  ~RpcBasicClient() {
    if (!moved && sock != -1) {
      close(sock);
    }
  }

 private:
  std::string ip;
  int port;

  int sock;
  struct sockaddr_in server;

  MoveIndicator moved;
};
}  // namespace dory::rpc
