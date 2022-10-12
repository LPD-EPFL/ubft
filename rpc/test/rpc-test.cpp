#include <chrono>

#include <gtest/gtest.h>

#include <dory/rpc/abstract-handler.hpp>
#include <dory/rpc/server.hpp>

#include <dory/shared/unused-suppressor.hpp>

using namespace dory;
using namespace rpc;

enum class RpcKind : uint8_t {
  RDMA_CONNECTION,
  JOIN,
  LEAVE,
};

class EstablishConnectionRpcHandler : public AbstractRpcHandler<RpcKind> {
 public:
  RpcKind kind() const override { return RpcKind::RDMA_CONNECTION; }

  void feed(uv_stream_t *client, ssize_t nread, char const *buf) override {
    dory::ignore(client);
    dory::ignore(nread);
    dory::ignore(buf);
  }

  void disconnected(uv_stream_t *client) override { dory::ignore(client); }
};

class JoinRpcHandler : public AbstractRpcHandler<RpcKind> {
 public:
  RpcKind kind() const override { return RpcKind::JOIN; }

  void feed(uv_stream_t *client, ssize_t nread, char const *buf) override {
    dory::ignore(client);
    dory::ignore(nread);
    dory::ignore(buf);
  }

  void disconnected(uv_stream_t *client) override { dory::ignore(client); }
};

class LeaveRpcHandler : public AbstractRpcHandler<RpcKind> {
 public:
  RpcKind kind() const override { return RpcKind::LEAVE; }

  void feed(uv_stream_t *client, ssize_t nread, char const *buf) override {
    dory::ignore(client);
    dory::ignore(nread);
    dory::ignore(buf);
  }

  void disconnected(uv_stream_t *client) override { dory::ignore(client); }
};

TEST(Rpc, RpcServer) { RpcServer<RpcKind> server("0.0.0.0", 7000); }

TEST(Rpc, AttachHandlers) {
  RpcServer<RpcKind> server("0.0.0.0", 7000);

  auto connection_handler = std::make_unique<EstablishConnectionRpcHandler>();
  auto join_handler = std::make_unique<JoinRpcHandler>();
  auto leave_handler = std::make_unique<LeaveRpcHandler>();

  server.attachHandler(std::move(connection_handler));
  server.attachHandler(std::move(join_handler));
  server.attachHandler(std::move(leave_handler));
}

TEST(Rpc, StartStopServerWithoutHandler1) {
  RpcServer<RpcKind> server("0.0.0.0", 7000);

  server.start();
  server.stop();
}

TEST(Rpc, StartStopServerWithoutHandler2) {
  RpcServer<RpcKind> server("0.0.0.0", 7000);

  server.start();
  std::this_thread::sleep_for(std::chrono::seconds(2));
  server.stop();
}

TEST(Rpc, StartServerWithHandler1) {
  RpcServer<RpcKind> server("0.0.0.0", 7000);

  auto connection_handler = std::make_unique<EstablishConnectionRpcHandler>();
  server.attachHandler(std::move(connection_handler));

  server.start();
  server.stop();
}

TEST(Rpc, StartServerWithHandler2) {
  RpcServer<RpcKind> server("0.0.0.0", 7000);

  auto connection_handler = std::make_unique<EstablishConnectionRpcHandler>();
  server.attachHandler(std::move(connection_handler));

  server.start();
  std::this_thread::sleep_for(std::chrono::seconds(2));
  server.stop();
}
