#pragma once

#include <cstring>
#include <string>

#include "../basic-client.hpp"

namespace dory::rpc::conn {
template <typename ProcIdType, typename RpcKind>
class ConnectionRpcClient : public RpcBasicClient {
 public:
  ConnectionRpcClient(std::string const &ip, int port)
      : RpcBasicClient(ip, port) {}

  bool sendRpc(RpcKind kind) {
    auto rpc_kind = static_cast<uint8_t>(kind);
    return send(&rpc_kind, sizeof(rpc_kind));
  }

  bool sendClientId(ProcIdType client_id) {
    return send(&client_id, sizeof(client_id));
  }

  bool sendConnectionInfo(std::string const &info) {
    auto length = static_cast<uint32_t>(info.size());
    if (!send(&length, sizeof(length))) {
      return false;
    }

    return send(info.data(), info.size());
  }

  bool recvConnectionInfo(std::string &str) {
    uint32_t length;

    auto buf = recv(sizeof(length));

    if (buf.size() < sizeof(length)) {
      return false;
    }

    std::memcpy(&length, buf.data(), sizeof(length));

    buf = recv(length);

    if (buf.size() < length) {
      return false;
    }

    for (auto c : buf) {
      str.push_back(c);
    }

    return true;
  }

  bool sendDone() {
    constexpr auto Len = std::char_traits<char>::length(Done);
    return send(Done, Len);
  }

  bool recvOk() {
    auto buf = recv(2);

    if (buf.size() < 2) {
      return false;
    }

    std::string ok;
    for (auto c : buf) {
      ok.push_back(c);
    }

    return ok == std::string("OK");
  }

 private:
  static constexpr char const *Done = "DONE";
};
}  // namespace dory::rpc::conn
