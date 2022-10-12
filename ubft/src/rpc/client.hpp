#pragma once

#include <cstddef>
#include <string>
#include <tuple>
#include <vector>

#include <fmt/core.h>
#include <fmt/ranges.h>

#include <hipony/enumerate.hpp>

#include <dory/ctrl/block.hpp>

#include <dory/memstore/store.hpp>

#include <dory/shared/logger.hpp>
#include <dory/shared/match.hpp>
#include <dory/third-party/sync/mpmc.hpp>

#include <dory/rpc/conn/universal-connector.hpp>

#include "../tail-p2p/types.hpp"
#include "../tail-queue/tail-queue.hpp"
#include "../thread-pool/tail-thread-pool.hpp"
#include "common.hpp"
#include "internal/common.hpp"
#include "internal/request.hpp"
#include "internal/response.hpp"

namespace dory::ubft::rpc {
class Client {
  using Request = internal::Request;
  using Response = internal::Response;
  using RpcConnectionClient =
      dory::rpc::conn::UniversalConnectionRpcClient<ProcId,
                                                    internal::RpcKind::Kind>;

 public:
  Client(Crypto& crypto, TailThreadPool& thread_pool, ctrl::ControlBlock& cb,
         ProcId const local_id, std::vector<ProcId> const server_ids,
         std::string const& identifier, size_t const window,
         size_t const max_request_size, size_t const max_response_size)
      : crypto{crypto},
        cb{cb},
        local_id{local_id},
        ns{fmt::format("rpc-{}-C{}", identifier, local_id)},
        window{window},
        max_request_size{max_request_size},
        max_full_request_size{Request::bufferSize(max_request_size)},
        max_full_signed_request_size{
            internal::SignedRequest::bufferSize(max_request_size)},
        max_full_response_size{Response::bufferSize(max_response_size)},
        request_pool{window + 1, max_full_request_size},
        response_pool{server_ids.size() * window, max_full_response_size},
        request_signing_pool{
            TailThreadPool::TaskQueue::maxOutstanding(window, thread_pool) + 1,
            max_full_request_size},
        requests{window},
        signature_computation{thread_pool, window} {
    for (auto const server_id : server_ids) {
      if (!connect(server_id)) {
        throw std::runtime_error(
            fmt::format("Could not connect to server {}.", server_id));
      }
    }
  }

  void tick() {
    for (auto& server : servers) {
      server.sender.tick();
    }
    pollResponses();

    if (slow_path) {
      pollSignatures();
      for (auto& server : servers) {
        server.sig_sender.tick();
      }
    }
  }

  /**
   * @brief Return a slot where to write a request and add it to the buffer to
   *        post.
   *
   * @param request_size
   * @return std::optional<uint8_t*> might be nullopt if no slot is available.
   */
  std::optional<uint8_t*> getSlot(size_t const request_size) {
    auto opt_buffer = request_pool.take(Request::bufferSize(request_size));
    if (!opt_buffer) {
      return std::nullopt;
    }
    auto& request = *reinterpret_cast<Request::Layout*>(opt_buffer->data());
    request.client_id = local_id;
    request.id = next_request++;
    request.size = request_size;

    requests_being_written.push_back(std::move(*opt_buffer));
    return requests_being_written.back().payload();
  }

  /**
   * @brief Post all requests that have been buffered via getSlot.
   *
   */
  void post() {
    for (auto&& request : requests_being_written) {
      auto& raw_request = request.rawBuffer();
      for (auto& server : servers) {
        auto* const slot = server.sender.getSlot(
            static_cast<tail_p2p::Size>(raw_request.size()));
        std::copy(raw_request.cbegin(), raw_request.cend(),
                  reinterpret_cast<uint8_t*>(slot));
      }
      auto const req_id = request.id();
      requests.tryEmplace(req_id, std::move(request), servers.size());
    }
    // We post all requests at once.
    for (auto& server : servers) {
      server.sender.send();
    }
    requests_being_written.clear();

    if (unlikely(slow_path)) {
      offloadSignatureComputations();
    }
  }

  std::optional<size_t> poll(uint8_t* dest) {
    if (requests.empty()) {
      return std::nullopt;
    }
    if (auto polled = requests.front().poll(dest)) {
      requests.popFront();
      return polled;
    }
    return std::nullopt;
  }

  void toggleSlowPath(bool const enable) {
    slow_path = enable;
    if (enable) {
      offloadSignatureComputations();
    }
  }

 private:
  void pollResponses() {
    for (auto&& [index, server] : hipony::enumerate(servers)) {
      // We only poll servers that haven't responded to all our requests.
      if (server.next_response >= next_request) {
        continue;
      }
      auto opt_borrow = response_pool.borrowNext();
      if (unlikely(!opt_borrow)) {
        throw std::logic_error("Response buffers be recycled.");
      }
      if (auto polled = server.receiver.poll(opt_borrow->get().data())) {
        auto response_ok = Response::tryFrom(*response_pool.take(*polled));
        match{response_ok}([](std::invalid_argument& error) { throw error; },
                           [&server = server, this](Response& response) {
                             server.next_response = response.requestId() + 1;
                             auto it = requests.find(response.requestId());
                             // std::vector<int>
                             // printable(response.stringView().begin(),
                             //                            response.stringView().end());
                             if (it == requests.end()) {
                               // fmt::print(
                               //     "Could not find request {} (response of
                               //     size {}: {})\n", response.requestId(),
                               //     response.size(), printable);
                               return;
                             }
                             it->second.newResponse(std::move(response));
                           });
      }
    }
  }

  bool connect(ProcId remote_id) {
    //// Create Senders & Receiver ////
    std::string const uuid(fmt::format("{}-R{}", ns, remote_id));

    // Unsigned request
    auto uuid_send = fmt::format("{}-send", uuid);
    cb.allocateBuffer(uuid_send,
                      Sender::bufferSize(window, max_full_request_size), 64);
    cb.registerMr(uuid_send, PdStandard, uuid_send, NoMemoryRights);
    cb.registerCq(uuid_send);

    conn::ReliableConnection rc_send(cb);
    rc_send.bindToPd(PdStandard);
    rc_send.bindToMr(uuid_send);
    rc_send.associateWithCq(uuid_send, uuid_send);
    rc_send.init(NoMemoryRights);

    // Signed request
    auto uuid_sig_send = fmt::format("{}-sig-send", uuid);
    cb.allocateBuffer(uuid_sig_send,
                      Sender::bufferSize(window, max_full_signed_request_size),
                      64);
    cb.registerMr(uuid_sig_send, PdStandard, uuid_sig_send, NoMemoryRights);
    cb.registerCq(uuid_sig_send);

    conn::ReliableConnection rc_sig_send(cb);
    rc_sig_send.bindToPd(PdStandard);
    rc_sig_send.bindToMr(uuid_sig_send);
    rc_sig_send.associateWithCq(uuid_sig_send, uuid_sig_send);
    rc_sig_send.init(NoMemoryRights);

    // Reply receiver
    auto const uuid_recv = fmt::format("{}-recv", uuid);
    cb.allocateBuffer(uuid_recv,
                      Receiver::bufferSize(window, max_full_response_size), 64);
    cb.registerMr(uuid_recv, PdStandard, uuid_recv, WriteMemoryRights);

    conn::ReliableConnection rc_recv(cb);
    rc_recv.bindToPd(PdStandard);
    rc_recv.bindToMr(uuid_recv);
    rc_recv.associateWithCq(CqUnused, CqUnused);
    rc_recv.init(WriteMemoryRights);

    auto [ip, port] = announcer.processToHost(remote_id);
    RpcConnectionClient cli(ip, port);

    cli.connect();
    auto [cli_ok, cli_offset_info] = cli.handshake<dory::uptrdiff_t>(
        [&rc_send, &rc_sig_send, &rc_recv]() -> std::pair<bool, std::string> {
          auto serialized_info =
              fmt::format("{} {} {}", rc_send.remoteInfo().serialize(),
                          rc_sig_send.remoteInfo().serialize(),
                          rc_recv.remoteInfo().serialize());
          return std::make_pair(true, serialized_info);
        },
        [&rc_send, &rc_sig_send, &rc_recv, remote_id](std::string const& info)
            -> std::pair<bool, std::optional<dory::uptrdiff_t>> {
          std::istringstream remote_info_stream(info);
          std::string rc_send_info;
          std::string rc_sig_send_info;
          std::string rc_recv_info;
          remote_info_stream >> rc_send_info;
          remote_info_stream >> rc_sig_send_info;
          remote_info_stream >> rc_recv_info;

          rc_send.reset();
          rc_send.reinit();
          rc_send.connect(conn::RemoteConnection::fromStr(rc_send_info),
                          remote_id);

          rc_sig_send.reset();
          rc_sig_send.reinit();
          rc_sig_send.connect(conn::RemoteConnection::fromStr(rc_sig_send_info),
                              remote_id);

          rc_recv.reset();
          rc_recv.reinit();
          rc_recv.connect(conn::RemoteConnection::fromStr(rc_recv_info),
                          remote_id);
          return std::make_pair(true, std::nullopt);
        },
        local_id, internal::RpcKind::RDMA_DYNAMIC_RPC_CONNECTION);

    if (!cli_ok) {
      LOGGER_WARN(logger, "Could not connect to process {}", remote_id);
      return false;
    }

    LOGGER_INFO(logger, "Connected to process {}", remote_id);

    servers.emplace_back(
        std::move(cli),
        Sender(window, max_full_request_size, std::move(rc_send)),
        Sender(window, max_full_signed_request_size, std::move(rc_sig_send)),
        Receiver(window, max_full_response_size, std::move(rc_recv)));

    return true;
  }

  void offloadSignatureComputations() {
    if (requests.empty()) {
      return;
    }
    // We jump over gaps
    next_to_offload = std::max(next_to_offload, requests.begin()->first);
    for (auto it = requests.find(next_to_offload); it != requests.end(); ++it) {
      auto& request = it->second.request;
      auto const& raw_req = request.rawBuffer();
      auto opt_buffer = request_signing_pool.take(raw_req.size());
      if (unlikely(!opt_buffer)) {
        throw std::runtime_error("RBSP buffers should be recycled.");
      }
      std::copy(raw_req.cbegin(), raw_req.cend(), opt_buffer->data());
      signature_computation.enqueue(
          [this, to_sign = std::move(*opt_buffer)]() mutable {
            // auto const ts = std::chrono::steady_clock::now();
            auto signature = crypto.sign(to_sign.data(), to_sign.size());
            // fmt::print("Computing the signature took: {}\n",
            // std::chrono::steady_clock::now() - ts);
            computed_signatures.enqueue(
                ComputedSignature{signature, std::move(to_sign)});
          });
      next_to_offload++;
    }
  }

  void pollSignatures() {
    size_t polled = 0;
    std::optional<ComputedSignature> cs;
    while ((cs.reset(), computed_signatures.try_dequeue(cs))) {
      polled++;
      auto const& [sig, raw_req] = *cs;
      for (auto& server : servers) {
        auto* const slot =
            server.sig_sender.getSlot(static_cast<tail_p2p::Size>(
                raw_req.size() + sizeof(Crypto::Signature)));
        auto* const sig_destp = std::copy(raw_req.cbegin(), raw_req.cend(),
                                          reinterpret_cast<uint8_t*>(slot));
        auto& sig_dest = *reinterpret_cast<Crypto::Signature*>(sig_destp);
        sig_dest = sig;
      }
      if (polled == window) {
        break;
      }
    }
    if (polled) {
      for (auto& server : servers) {
        server.sig_sender.send();
      }
    }
  }

  Crypto& crypto;
  ctrl::ControlBlock& cb;
  ProcId const local_id;
  std::string const ns;
  memstore::ProcessAnnouncer announcer;

  struct Server {
    Server(RpcConnectionClient&& cli, Sender&& sender, Sender&& sig_sender,
           Receiver&& receiver)
        : cli{std::move(cli)},
          sender{std::move(sender)},
          sig_sender{std::move(sig_sender)},
          receiver{std::move(receiver)} {}

    RpcConnectionClient cli;
    RequestId next_response = 0;
    Sender sender;
    Sender sig_sender;
    Receiver receiver;
  };

  std::vector<Server> servers;

  size_t const window;
  size_t const max_request_size;
  size_t const max_full_request_size;
  size_t const max_full_signed_request_size;
  size_t const max_full_response_size;
  RequestId next_request = 0;
  bool posted = false;
  bool slow_path = false;
  RequestId next_to_offload = 0;

  static auto constexpr PdStandard = "standard";
  static auto constexpr CqUnused = "unused";

  static auto constexpr NoMemoryRights = ctrl::ControlBlock::LOCAL_READ;

  static auto constexpr WriteMemoryRights =
      ctrl::ControlBlock::LOCAL_READ | ctrl::ControlBlock::LOCAL_WRITE |
      ctrl::ControlBlock::REMOTE_READ | ctrl::ControlBlock::REMOTE_WRITE;

  struct RequestData {
    static size_t constexpr MaxNbServers = 5;
    RequestData(Request&& request, size_t const nb_servers)
        : request{std::move(request)}, quorum{(nb_servers / 2) + 1} {}

    void newResponse(Response&& response) {
      all_identical = !responses.front() ||
                      (all_identical && response == *responses.front());
      responses[nb_responses++].emplace(std::move(response));
    }

    std::optional<size_t> poll(uint8_t* dest) {
      if (nb_responses < quorum) {
        return std::nullopt;
      }
      if (all_identical) {
        auto const& response = *responses.front();
        std::copy(response.begin(), response.end(), dest);
        return response.size();
      }
      // Slow path, we need to check if we got a majority.
      throw std::logic_error("Byzantine behavior, responses did not match.");
    }

    Request request;
    size_t const quorum;
    size_t nb_responses = 0;
    std::array<std::optional<Response>, MaxNbServers> responses;
    bool all_identical = true;
  };

  Pool request_pool;
  Pool response_pool;
  Pool request_signing_pool;
  TailMap<RequestId, RequestData> requests;
  std::deque<Request> requests_being_written;

  struct ComputedSignature {
    Crypto::Signature signature;
    Buffer buffer;  // so that is is returned to the main thread
  };

  third_party::sync::MpmcQueue<ComputedSignature> computed_signatures;
  TailThreadPool::TaskQueue signature_computation;

  LOGGER_DECL_INIT(logger, "ClientRpc");
};
}  // namespace dory::ubft::rpc
