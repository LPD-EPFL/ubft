#pragma once

#include <cstdint>
#include <functional>

namespace dory::ubft::rpc::internal {

struct RpcKind {
  enum Kind : uint8_t {
    RDMA_DYNAMIC_RPC_CONNECTION,
  };

  static char const* name(Kind k) {
    switch (k) {
      case RDMA_DYNAMIC_RPC_CONNECTION:
        return "RdmaDynamicRpcConnectionHandler";
      default:
        throw std::runtime_error("Unknown RpcKind name!");
    }
  }
};

// TODO: move to shared/types.hpp
template <typename T>
using Ref = std::reference_wrapper<T>;
template <typename T>
using ConstRef = std::reference_wrapper<T const>;
template <typename T>
using OptionalRef = std::optional<Ref<T>>;
template <typename T>
using OptionalConstRef = std::optional<ConstRef<T>>;

}  // namespace dory::ubft::rpc::internal
