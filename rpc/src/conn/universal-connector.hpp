#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "rpc-request.hpp"

namespace dory::rpc::conn {

template <typename ProcIdType, typename RpcKind>
class UniversalConnectionRpcClient
    : public ConnectionRpcClient<ProcIdType, RpcKind> {
 public:
  UniversalConnectionRpcClient(std::string const& ip, int port)
      : ConnectionRpcClient<ProcIdType, RpcKind>(ip, port) {}

  using SerializeConnectionType = std::function<std::pair<bool, std::string>()>;

  template <typename RetType>
  using SetupConnectionType =
      std::function<std::pair<bool, std::optional<RetType>>(
          std::string const& info)>;

  template <typename RetType>
  std::pair<bool, std::optional<RetType>> handshake(
      SerializeConnectionType const& serialize_connection,
      SetupConnectionType<RetType> const& setup_connection, ProcIdType id,
      RpcKind kind) {
    if (!this->sendRpc(kind)) {
      return std::make_pair(false, std::nullopt);
    }

    if (!this->sendClientId(id)) {
      return std::make_pair(false, std::nullopt);
    }

    auto [serialization_ok, serialized_info] = serialize_connection();
    if (!serialization_ok || !this->sendConnectionInfo(serialized_info)) {
      return std::make_pair(false, std::nullopt);
    }

    std::string remote_info;
    if (!this->recvConnectionInfo(remote_info)) {
      return std::make_pair(false, std::nullopt);
    }

    auto [connected, connection_info] = setup_connection(remote_info);
    if (!connected) {
      return std::make_pair(false, std::nullopt);
    }

    if (!this->sendDone()) {
      return std::make_pair(false, std::nullopt);
    }

    if (!this->recvOk()) {
      return std::make_pair(false, std::nullopt);
    }

    return std::make_pair(true, connection_info);
  }
};
}  // namespace dory::rpc::conn
