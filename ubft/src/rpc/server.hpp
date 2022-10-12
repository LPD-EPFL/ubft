#pragma once

#include <memory>
#include <optional>

#include <fmt/core.h>
#include <hipony/enumerate.hpp>

#include <dory/ctrl/block.hpp>

#include <dory/memstore/store.hpp>

#include <dory/shared/logger.hpp>
#include <dory/shared/match.hpp>
#include <dory/shared/types.hpp>

#include <dory/rpc/conn/universal-connector.hpp>

// #include <dory/crash-consensus.hpp>

#include "../buffer.hpp"
#include "../crypto.hpp"
#include "../helpers.hpp"
#include "../thread-pool/tail-thread-pool.hpp"
#include "../types.hpp"
#include "common.hpp"
#include "internal/common.hpp"
#include "internal/connection.hpp"
#include "internal/ingress.hpp"
#include "internal/request.hpp"
#include "internal/response.hpp"

#include "../tail-p2p/receiver-builder.hpp"
#include "../tail-p2p/sender-builder.hpp"

namespace dory::ubft::rpc {
class Server {
  using RpcConnectionServer = dory::rpc::RpcServer<internal::RpcKind::Kind>;
  using DynamicConnections = internal::Manager::DynamicConnections;
  using Connection = internal::Connection;

 public:
  using Request = internal::Request;
  using SignedRequest = internal::SignedRequest;
  using Response = internal::Response;

  Server(Crypto &crypto, TailThreadPool &thread_pool, ctrl::ControlBlock &cb,
         ProcId const local_id, std::string const &identifier,
         ProcId const min_client_id, ProcId const max_client_id,
         size_t const window, size_t const max_request_size,
         size_t const max_response_size, size_t const max_connections,
         size_t const server_window, std::vector<ProcId> const &server_ids)
      : cb{cb},
        store{dory::memstore::MemoryStore::getInstance()},
        local_id{local_id},
        ns{fmt::format("rpc-{}-S{}", identifier, local_id)},
        min_client_id{min_client_id},
        window{window},
        max_request_size{max_request_size},
        max_response_size{max_response_size},
        server_window{server_window},
        server_ids{move_back(server_ids, local_id)},
        leader_id{*std::min_element(this->server_ids.begin(),
                                    this->server_ids.end())},
        leader_index{static_cast<size_t>(
            find(this->server_ids.begin(), this->server_ids.end(), leader_id) -
            this->server_ids.begin())},
        rpc_connection_server{buildRpcConnectionServer(
            cb, local_id, window, max_request_size, max_response_size,
            max_connections, dynamic_connections)},
        request_pool{(max_client_id - min_client_id + 1) * (window + 1),
                     Request::bufferSize(max_request_size)},
        signed_request_pool{
            (max_client_id - min_client_id + 1 + (server_ids.size() - 1)) *
                (window + 1),
            SignedRequest::bufferSize(max_request_size)},
        echo_pool{(server_ids.size() - 1) *
                      (max_client_id - min_client_id + 1) * (window + 1),
                  Request::bufferSize(max_request_size)},
        ingress{crypto,        thread_pool, min_client_id,
                max_client_id, window,      server_ids.size()},
        clients{static_cast<size_t>(max_client_id - min_client_id + 1)} {
    announcer.announceProcess(local_id, rpc_connection_server->port());
    connectServers(server_ids);
  }

  void tick() {
    if ((ticks++ % (1 << 10)) == 0) {
      updateConnections();
    }
    if (likely(!slow_path)) {  // FAST PATH
      pollClientRequests();
      for (auto &server : servers) {
        server.request_sender.tick();
        server.ack_sender.tick();
      }
      if (!optimistic) {
        if (leader_id == local_id) {
          pollFollowerEchoes();
        } else {
          echoToLeader();
        }
      }
    } else {  // SLOW PATH
      pollClientSignedRequests();
      ingress.tick();
      for (auto &server : servers) {
        server.sig_sender.tick();  // Leader signed echo
        server.ack_sender.tick();  // Leader signed echo backpressure
      }
      if (!optimistic) {
        if (leader_id == local_id) {
          echoToFollowers();
        } else {
          pollLeaderSignedEchoes();
        }
      }
    }
  }

  void toggleSlowPath(bool const enable) { slow_path = enable; }

  void toggleOptimism(bool const optimism) { optimistic = optimism; }

  /**
   * @brief Return requests that where received from clients so that
   *        acceptRequest can be called on consensus.
   *        A request will not be echoed to the leader before being returned by
   *        this method.
   */
  internal::OptionalConstRef<Request> pollReceived() {
    return ingress.pollReceived();
  }

  /**
   * @brief Return requests that where echoed by followers (if !optimistic) or
   *        simply received (if optimistic) so that they can be forwarded to
   *        consensus.
   */
  internal::OptionalConstRef<Request> pollProposable() {
    return ingress.pollProposable(!slow_path, optimistic);
  }

  /**
   * @brief Called after a value is decided to respond to the client
   *
   * @param client_id
   * @param request_id
   * @param response
   * @param response_size
   */
  void executed(ProcId const client_id, RequestId const request_id,
                uint8_t const* const response, size_t const response_size) {
    auto client = getClient(client_id);
    if (unlikely(!client || !client->active)) {
      LOGGER_WARN(logger,
                  "Executed a command from client {} which is not connected or "
                  "not active.",
                  client_id);
      return;
    }
    ingress.executed(client_id, request_id);
    if (unlikely(local_id == 3)) {
      // The last replica never replies, to simulate the real latency.
      return;
    }
    auto &client_sender = client->data->sender;
    auto const buffer_size =
        static_cast<tail_p2p::Size>(Response::bufferSize(response_size));
    auto &slot = *reinterpret_cast<Response::Layout *>(
        client_sender.getSlot(buffer_size));
    slot.request_id = request_id;
    std::copy(response, response + response_size, &slot.response);
    client_sender.send();
    LOGGER_DEBUG(logger, "Replied to client #{} about request #{}.", client_id,
                 request_id);
  }

  void setLeader(ProcId const new_leader) {
    leader_id = new_leader;
    leader_index = find(server_ids.begin(), server_ids.end(), leader_id) -
                   server_ids.begin();
  }

 private:
  inline void updateConnections() {
    connections.emplace(dynamic_connections->get().connections());
  }

  void pollClientRequests() {
    for (auto &[proc_id, conn] : connections->get()) {
      auto &[active, client] = conn;
      if (!*active) {
        continue;
      }
      client->sender.tick();
      auto opt_borrowed_buffer = request_pool.borrowNext();
      if (unlikely(!opt_borrowed_buffer)) {
        throw std::logic_error("Request buffers should be recycled.");
      }
      if (auto const polled =
              client->receiver.poll(opt_borrowed_buffer->get().data())) {
        auto buffer = *request_pool.take(*polled);
        auto request = Request::tryFrom(std::move(buffer));
        match{request}([](std::invalid_argument &err) { throw err; },
                       [this, proc_id, &conn](Request &request) {
                         handleRequest(proc_id, std::move(request), conn);
                       });
      }
    }
  }

  void handleRequest(ProcId const from_id, Request &&request,
                     Connection &conn) {
    if (from_id != request.clientId()) {
      throw std::runtime_error(
          "Byzantine behavior, received a client request with invalid id.");
    }
    ingress.fromClient(std::move(request));
    auto &client = getClient(from_id);
    if (!client) {
      client.emplace(conn);
    }
  }

  void pollClientSignedRequests() {
    for (auto &[proc_id, conn] : connections->get()) {
      auto &[active, client] = conn;
      if (!*active) {
        continue;
      }
      client->sender.tick();
      auto opt_borrowed_buffer = signed_request_pool.borrowNext();
      if (unlikely(!opt_borrowed_buffer)) {
        throw std::logic_error("Request buffers should be recycled.");
      }
      if (auto const polled =
              client->sig_receiver.poll(opt_borrowed_buffer->get().data())) {
        auto buffer = *signed_request_pool.take(*polled);
        auto request = SignedRequest::tryFrom(std::move(buffer));
        match{request}([](std::invalid_argument &err) { throw err; },
                       [this, proc_id, &conn](SignedRequest &request) {
                         handleRequest(proc_id, std::move(request), conn);
                       });
      }
    }
  }

  void handleRequest(ProcId const from_id, SignedRequest &&request,
                     Connection &conn) {
    if (from_id != request.clientId()) {
      throw std::runtime_error(
          "Byzantine behavior, received a signed client request with invalid "
          "id.");
    }
    ingress.fromClient(std::move(request));
    auto &client = getClient(from_id);
    if (!client) {
      client.emplace(conn);
    }
  }

  void pollLeaderSignedEchoes() {
    size_t echoes_polled = 0;
    size_t constexpr MaxPolls = 3;
    auto &leader = servers.at(leader_index);
    for (; echoes_polled < MaxPolls; echoes_polled++) {
      auto opt_borrowed_buffer = signed_request_pool.borrowNext();
      if (unlikely(!opt_borrowed_buffer)) {
        throw std::logic_error("Request buffers should be recycled.");
      }
      if (auto const polled =
              leader.sig_receiver.poll(opt_borrowed_buffer->get().data())) {
        auto buffer = *signed_request_pool.take(*polled);
        auto request = SignedRequest::tryFrom(std::move(buffer));
        match{request}([](std::invalid_argument &err) { throw err; },
                       [this](SignedRequest &request) {
                         ingress.fromLeader(std::move(request));
                       });
      } else {
        break;
      }
    }
    if (echoes_polled != 0) {
      auto *slot = leader.ack_sender.getSlot(sizeof(OtherServer::Ack));
      *reinterpret_cast<OtherServer::Ack *>(slot) = echoes_polled;
      leader.ack_sender.send();
    }
  }

  void echoToFollowers() {
    for (auto &&[to, server] : hipony::enumerate(servers)) {
      echoToFollower(to, server);
    }
  }

  struct OtherServer;
  void echoToFollower(size_t const to, OtherServer &server) {
    if (server.outstanding_requests > 0) {
      OtherServer::Ack ack;
      while (auto const polled = server.ack_receiver.poll(&ack)) {
        if (*polled != sizeof(ack)) {
          throw std::runtime_error("Byzantine behavior: ack of invalid size.");
        }
        if (ack > server.outstanding_requests) {
          throw std::runtime_error("Byzantine behavior: acked too much.");
        }
        server.outstanding_requests -= ack;
      }
    }
    // We echo the messages we received from clients.
    size_t echoed = 0;
    while (server.outstanding_requests < server_window) {
      if (auto const to_forward = ingress.pollToForward(to)) {
        // fmt::print("Found something to echo!\n");
        auto const &[request, signature] = *to_forward;

        auto const &raw_req = request.get().rawBuffer();
        auto const bsize = static_cast<tail_p2p::Size>(
            raw_req.size() + sizeof(Crypto::Signature));

        auto *slot = server.sig_sender.getSlot(bsize);
        std::copy(raw_req.cbegin(), raw_req.cend(),
                  reinterpret_cast<uint8_t *>(slot));

        // We append the signature at the end of the request.
        auto *const sig_destp = std::copy(raw_req.cbegin(), raw_req.cend(),
                                          reinterpret_cast<uint8_t *>(slot));
        auto &sig_dest = *reinterpret_cast<Crypto::Signature *>(sig_destp);
        sig_dest = signature;

        echoed++;
        server.outstanding_requests++;
      } else {
        break;
      }
    }
    if (echoed > 0) {
      // fmt::print("Echoed {} signed requests to follower idx {}\n", echoed,
      // to);
      server.sig_sender.send();
    }
  }

  void pollFollowerEchoes() {
    for (auto &&[from, server] : hipony::enumerate(servers)) {
      size_t echoes_polled = 0;
      size_t constexpr MaxPolls = 3;
      for (; echoes_polled < MaxPolls; echoes_polled++) {
        auto opt_borrowed_buffer = echo_pool.borrowNext();
        if (unlikely(!opt_borrowed_buffer)) {
          throw std::logic_error("Echo buffers should be recycled.");
        }
        auto const polled =
            server.request_receiver.poll(opt_borrowed_buffer->get().data());
        if (!polled) {
          break;
        }
        auto buffer = *echo_pool.take(*polled);
        auto echo = Request::tryFrom(std::move(buffer));
        match{echo}([](std::invalid_argument &err) { throw err; },
                    [this, from = from](Request &echo) {
                      handleEcho(from, std::move(echo));
                    });
      }
      if (echoes_polled != 0) {
        auto *slot = server.ack_sender.getSlot(sizeof(OtherServer::Ack));
        *reinterpret_cast<OtherServer::Ack *>(slot) = echoes_polled;
        server.ack_sender.send();
      }
    }
  }

  void handleEcho(size_t const from, Request &&echo) {
    ingress.fromFollower(std::move(echo), from);
  }

  void echoToLeader() {
    auto &leader = servers.at(leader_index);
    // We receive acks.
    if (leader.outstanding_requests > 0) {
      OtherServer::Ack ack;
      while (auto const polled = leader.ack_receiver.poll(&ack)) {
        if (*polled != sizeof(ack)) {
          throw std::runtime_error("Byzantine behavior: ack of invalid size.");
        }
        if (ack > leader.outstanding_requests) {
          throw std::runtime_error("Byzantine behavior: acked too much.");
        }
        leader.outstanding_requests -= ack;
      }
    }
    // We echo the messages we received from clients.
    size_t echoed = 0;
    while (leader.outstanding_requests < server_window) {
      if (auto const request = ingress.pollToEcho(leader_index)) {
        auto const &raw_request = request->get().rawBuffer();
        auto const bsize = static_cast<tail_p2p::Size>(raw_request.size());
        auto *slot = leader.request_sender.getSlot(bsize);
        std::copy(raw_request.cbegin(), raw_request.cend(),
                  reinterpret_cast<uint8_t *>(slot));
        echoed++;
        leader.outstanding_requests++;
      } else {
        break;
      }
    }
    if (echoed > 0) {
      leader.request_sender.send();
    }
  }

  void connectServers(std::vector<ProcId> const &replica_ids) {
    if (replica_ids.size() == 1 && replica_ids.front() == local_id) {
      return; // Do not connect to any server
    }

    std::vector<tail_p2p::AsyncSenderBuilder> request_sender_builders;
    std::vector<tail_p2p::AsyncSenderBuilder> sig_request_sender_builders;
    std::vector<tail_p2p::AsyncSenderBuilder> ack_sender_builders;
    std::vector<tail_p2p::ReceiverBuilder> request_receiver_builders;
    std::vector<tail_p2p::ReceiverBuilder> sig_request_receiver_builders;
    std::vector<tail_p2p::ReceiverBuilder> ack_receiver_builders;
    for (auto const server_id : replica_ids) {
      if (server_id == local_id) {
        continue;
      }
      request_sender_builders.emplace_back(
          cb, local_id, server_id, "server-group-request", server_window,
          Request::bufferSize(max_request_size));
      request_sender_builders.back().announceQps();
      request_receiver_builders.emplace_back(
          cb, local_id, server_id, "server-group-request", server_window,
          Request::bufferSize(max_request_size));
      request_receiver_builders.back().announceQps();
      sig_request_sender_builders.emplace_back(
          cb, local_id, server_id, "server-group-sig-request", server_window,
          SignedRequest::bufferSize(max_request_size));
      sig_request_sender_builders.back().announceQps();
      sig_request_receiver_builders.emplace_back(
          cb, local_id, server_id, "server-group-sig-request", server_window,
          SignedRequest::bufferSize(max_request_size));
      sig_request_receiver_builders.back().announceQps();
      ack_sender_builders.emplace_back(cb, local_id, server_id,
                                       "server-group-ack", server_window,
                                       sizeof(OtherServer::Ack));
      ack_sender_builders.back().announceQps();
      ack_receiver_builders.emplace_back(cb, local_id, server_id,
                                         "server-group-ack", server_window,
                                         sizeof(OtherServer::Ack));
      ack_receiver_builders.back().announceQps();
    }

    store.barrier("server_group_qp_announced", replica_ids.size());

    for (auto &builder : request_sender_builders) {
      builder.connectQps();
    }
    for (auto &builder : request_receiver_builders) {
      builder.connectQps();
    }
    for (auto &builder : sig_request_sender_builders) {
      builder.connectQps();
    }
    for (auto &builder : sig_request_receiver_builders) {
      builder.connectQps();
    }
    for (auto &builder : ack_sender_builders) {
      builder.connectQps();
    }
    for (auto &builder : ack_receiver_builders) {
      builder.connectQps();
    }

    store.barrier("server_group_qp_connected", replica_ids.size());

    auto request_sender_builder_it = request_sender_builders.begin();
    auto request_receiver_builder_it = request_receiver_builders.begin();
    auto sig_request_sender_builder_it = sig_request_sender_builders.begin();
    auto sig_request_receiver_builder_it =
        sig_request_receiver_builders.begin();
    auto ack_sender_builder_it = ack_sender_builders.begin();
    auto ack_receiver_builder_it = ack_receiver_builders.begin();
    for (auto const server_id : replica_ids) {
      if (server_id == local_id) {
        continue;
      }
      servers.emplace_back(server_id, request_sender_builder_it->build(),
                           request_receiver_builder_it->build(),
                           sig_request_sender_builder_it->build(),
                           sig_request_receiver_builder_it->build(),
                           ack_sender_builder_it->build(),
                           ack_receiver_builder_it->build());

      request_sender_builder_it++;
      request_receiver_builder_it++;
      sig_request_sender_builder_it++;
      sig_request_receiver_builder_it++;
      ack_sender_builder_it++;
      ack_receiver_builder_it++;
    }

    store.barrier("server_group_abstractions_initialized", replica_ids.size());
  }

  static std::unique_ptr<RpcConnectionServer> buildRpcConnectionServer(
      ctrl::ControlBlock &cb, ProcId const local_id, size_t const window,
      size_t const max_request_size, size_t const max_response_size,
      size_t const max_connections, DelayedRef<DynamicConnections> &client_dc) {
    LOGGER_DECL_INIT(logger, "RpcConnectionServerBuilder");

    auto manager = std::make_unique<internal::Manager>(
        cb, local_id, window, Response::bufferSize(max_response_size),
        Request::bufferSize(max_request_size), max_connections);
    client_dc.emplace(manager->connections());
    auto handler = std::make_unique<internal::Handler>(
        std::move(manager), internal::RpcKind::RDMA_DYNAMIC_RPC_CONNECTION);

    LOGGER_INFO(logger, "Setup of the RPC server");
    auto rpc_connection_server =
        std::make_unique<RpcConnectionServer>("0.0.0.0", 7000);
    rpc_connection_server->attachHandler(std::move(handler));

    LOGGER_INFO(logger, "Starting the RPC server");
    rpc_connection_server->startOrChangePort();
    return rpc_connection_server;
  }

  inline std::optional<Connection> &getClient(ProcId const client_id) {
    return clients[client_id - min_client_id];
  }

  ctrl::ControlBlock &cb;
  dory::memstore::MemoryStore &store;
  ProcId const local_id;
  std::string const ns;

  ProcId const min_client_id;
  size_t const window;
  size_t const max_request_size;
  size_t const max_response_size;
  size_t const server_window;
  std::vector<ProcId> server_ids;
  ProcId leader_id;
  size_t leader_index;
  bool slow_path = false;
  bool optimistic = false;
  size_t ticks = 0;

  memstore::ProcessAnnouncer announcer;
  DelayedRef<DynamicConnections> dynamic_connections;
  DelayedRef<std::vector<DynamicConnections::ValueType>> connections;
  std::unique_ptr<RpcConnectionServer> rpc_connection_server;

  Pool request_pool;
  Pool signed_request_pool;
  Pool echo_pool;
  internal::RequestIngress ingress;

  struct OtherServer {
    using Ack = size_t;
    OtherServer(ProcId const proc_id, Sender &&request_sender,
                Receiver &&request_receiver, Sender &&sig_sender,
                Receiver &&sig_receiver, Sender &&ack_sender,
                Receiver &&ack_receiver)
        : proc_id{proc_id},
          request_sender{std::move(request_sender)},
          request_receiver{std::move(request_receiver)},
          sig_sender{std::move(sig_sender)},
          sig_receiver{std::move(sig_receiver)},
          ack_sender{std::move(ack_sender)},
          ack_receiver{std::move(ack_receiver)} {}

    ProcId proc_id;
    Sender request_sender;
    Receiver request_receiver;
    Sender sig_sender;
    Receiver sig_receiver;

    // Acks for backpressure
    Sender ack_sender;
    Receiver ack_receiver;

    size_t outstanding_requests = 0;
  };

  std::vector<OtherServer> servers;
  std::vector<std::optional<Connection>> clients;

  LOGGER_DECL_INIT(logger, "ServerRpc");
};
}  // namespace dory::ubft::rpc
