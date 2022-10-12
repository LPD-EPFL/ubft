#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <vector>

#include <fmt/core.h>
#include <hipony/enumerate.hpp>

#include <dory/shared/dynamic-bitset.hpp>
#include <dory/shared/logger.hpp>
#include <dory/third-party/sync/mpmc.hpp>

#include "../../tail-map/tail-map.hpp"
#include "../../tail-p2p/sender.hpp"
#include "../../tail-queue/tail-queue.hpp"
#include "../../types.hpp"
#include "common.hpp"
#include "request.hpp"

#include "../../latency-hooks.hpp"

namespace dory::ubft::rpc::internal {

class RequestStateMachine {
 public:
  using Sender = tail_p2p::internal::AsyncSender;

  RequestStateMachine(Request &&request, size_t const unanimity_size,
                      std::optional<Crypto::Signature> signature = std::nullopt)
      : req{std::move(request)},
        unanimity_size{unanimity_size},
        echoes(unanimity_size),
        signature{signature} {
    echoes.set(unanimity_size - 1);
  }

  bool echoed(Request &echo, size_t const follower_idx) {
    LOGGER_DEBUG(logger, "Follower {} echoed {} from {}.", follower_idx,
                 echo.id(), echo.clientId());

    if (echoes.get(follower_idx)) {
      LOGGER_WARN(logger, "Follower echoed twice.");
      return false;
    }

    if (echo != req) {
      LOGGER_WARN(logger, "Echo does not match the original request.");
      return false;
    }

    echoes.set(follower_idx);
    return true;
  }

  bool checkedSignature(Crypto::Signature const &sig) {
    bool const ret = !signature.has_value();
    signature = sig;
    return ret;
  }

  std::optional<Crypto::Signature> const &getSignature() const {
    return signature;
  }

  bool echoed() const { return echoes.full(); }

  Request extract() { return std::move(req); }
  Request const &get() const { return req; }

  bool proposable(bool const fast_path, bool const optimistic) {
    // In the optimistic case, we don't wait for any acknowledgement.
    if (optimistic) {
      return true;
    }
    // In the fast path, the message must have been echoed by everyone.
    if (fast_path) {
      return echoed();
    }
    // In the slow path, we just need to make sure that we have the signature
    // that replicas are (eventually) gonna receive (except if they vote for
    // leader change before).
    return !!getSignature();
  }

 private:
  Request req;                  // Raw request
  size_t const unanimity_size;  // Myself plus the number of followers
  DynamicBitset echoes;  // How many processes have seen the same request (it
                         // needs to match), including the leader
  std::optional<Crypto::Signature> signature;

  LOGGER_DECL_INIT(logger, "RpcRequestStateMachine");
};

class RequestIngress {
  class ClientRequestIngress {
    struct VerifiedSignature {
      Request request;
      Crypto::Signature signature;
      bool valid;
    };

   public:
    ClientRequestIngress(Crypto &crypto, TailThreadPool &thread_pool,
                         ProcId const id, size_t const window,
                         size_t const nb_followers)
        : pollable_below{window},
          id{id},
          crypto{crypto},
          client_signature_verification{thread_pool, window},
          leader_signature_verification{thread_pool, window},
          window{window},
          requests{window} {
      for (size_t i = 0; i < nb_followers; i++) {
        buffered_requests.emplace_back(window);
        next_poll_to_echo.push_back(0);
        next_poll_to_forward.push_back(0);
      }
    }

    void fromFollower(Request &&req, size_t const follower_index) {
      auto req_it = requests.find(req.id());
      if (req_it == requests.end()) {
        LOGGER_DEBUG(logger,
                     "Received a request asynchronously from a follower.");
        buffered_requests.at(follower_index).emplaceBack(std::move(req));
        return;
      }
      req_it->second.echoed(req, follower_index);
    }

    void fromClient(Request &&req, size_t const unanimity_size) {
      auto const req_id = req.id();
      if (unlikely(requests.find(req_id) != requests.end())) {
        throw std::runtime_error(
            "Byzantine behavior: client sent request twice.");
      }
      try {
        requests.tryEmplace(req_id, std::move(req), unanimity_size);
      } catch (const std::exception &e) {
        throw std::runtime_error(
            "Byzantine behavior: client re-sent a past request.");
      }

      for (auto &&[follower, buffer] : hipony::enumerate(buffered_requests)) {
        while (!buffer.empty() && buffer.front().id() < req_id) {
          buffer.popFront();
        }
        if (!buffer.empty() && buffer.front().id() == req_id) {
          auto buffered_request = std::move(buffer.front());
          buffer.popFront();
          fromFollower(std::move(buffered_request), follower);
        }
      }
    }

    void fromClient(SignedRequest &&req) {
      // fmt::print("Received signed request #{} from client...\n", req.id());
      enqueueSignatureVerification(std::move(req),
                                   client_signature_verification);
    }

    void fromLeader(SignedRequest &&req) {
      // fmt::print("Received signed request #{} from leader...\n", req.id());
      enqueueSignatureVerification(std::move(req),
                                   leader_signature_verification);
    }

    void pollVerifiedSignatures(size_t const unanimity_size) {
      std::optional<VerifiedSignature> vs;
      while ((vs.reset(), verified_signatures.try_dequeue(vs))) {
        #ifdef LATENCY_HOOKS
          hooks::sig_check_latency.addMeasurement(hooks::Clock::now() - hooks::sig_check_start);
        #endif
        auto &[req, sig, valid] = *vs;
        if (!valid) {
          throw std::runtime_error(
              "Byzantine Behavior: invalid signature received.");
        }
        // fmt::print("Verified {}\n", req.id());
        auto const req_id = req.id();
        auto req_it = requests.find(req_id);
        if (req_it == requests.end()) {
          // TODO: deal with asynchrony, requests arriving out of order.
          try {
            requests.tryEmplace(req_id, std::move(req), unanimity_size, sig);
          } catch (const std::exception &e) {
            LOGGER_WARN(logger, "TODO: Signatures verified OoO, discarded.");
            // There is a fundamental issue in receiving requests from different
            // sources in different orders. This can be fixed by enforcing FIFO
            // and using checkpoints.
          }
        } else {
          req_it->second.checkedSignature(sig);
        }
      }
    }

    OptionalConstRef<Request> pollReceived() {
      if (requests.empty()) {
        return std::nullopt;
      }

      // We jump over gaps.
      auto const first_id = requests.begin()->first;
      if (first_id > next_poll_received) {
        next_poll_received = first_id;
      }

      auto const it = requests.find(next_poll_received);
      if (it == requests.end()) {
        return std::nullopt;
      }

      // This prevents overflowing the consensus layer by not polling requests
      // that would go out of the decision window. However (TODO), this prevents
      // from participating in consensus if not all messages were delivered.
      // This "contradicts" the "jump over gaps" above.
      // TODO: call executed() upon checkpoints.
      if (unlikely(it->first >= pollable_below)) {
        return std::nullopt;
      }
      next_poll_received++;
      return it->second.get();
    }
    RequestId pollable_below;
    RequestId next_poll_received = 0;

    OptionalConstRef<Request> pollToEcho(size_t const leader_index) {
      if (requests.empty()) {
        return std::nullopt;
      }
      auto &npte = next_poll_to_echo.at(leader_index);

      // We jump over gaps.
      auto const first_id = requests.begin()->first;
      if (first_id > npte) {
        npte = first_id;
      }

      // We can only echo messages that have been pollReceived.
      if (npte >= next_poll_received) {
        return std::nullopt;
      }

      auto const it = requests.find(npte);
      if (it == requests.end()) {
        return std::nullopt;
      }
      npte++;
      return it->second.get();
    }
    std::vector<RequestId> next_poll_to_echo;

    std::optional<std::pair<ConstRef<Request>, ConstRef<Crypto::Signature>>>
    pollToForward(size_t const dest_index) {
      if (requests.empty()) {
        return std::nullopt;
      }
      auto &nptf = next_poll_to_forward.at(dest_index);

      // We jump over gaps.
      auto const first_id = requests.begin()->first;
      if (first_id > nptf) {
        nptf = first_id;
      }

      // We can only forward messages if we have a signature for them.
      auto it = requests.find(nptf);
      // TODO: work when transitionning between fast and slow path as for now
      // it blocks with old requests not having signatures.
      if (it == requests.end() || !it->second.getSignature()) {
        fmt::format("did not forward: {}\n", !!it->second.getSignature());
        return std::nullopt;
      }
      nptf++;
      return std::make_pair(std::cref(it->second.get()),
                            std::cref(*it->second.getSignature()));
    }
    std::vector<RequestId> next_poll_to_forward;

    OptionalConstRef<Request> pollProposable(bool const fast_path,
                                             bool const optimistic) {
      if (requests.empty()) {
        return std::nullopt;
      }

      // We jump over gaps.
      auto const first_id = requests.begin()->first;
      if (first_id > next_poll_proposable) {
        next_poll_proposable = first_id;
      }

      // We can only propose messages that have been received (=accepted)
      // locally. This could be removed by automatically accepting proposed
      // messages.
      if (next_poll_proposable >= next_poll_received) {
        return std::nullopt;
      }

      auto const it = requests.find(next_poll_proposable);
      // On the fast path, we wait for all followers to have echoed.
      // On the slow path, we have to propose even if a request was echoed by
      // only f+1 replicas as we cannot wait for more.
      if (it == requests.end() ||
          !it->second.proposable(fast_path, optimistic)) {
        return std::nullopt;
      }
      next_poll_proposable++;
      return it->second.get();
    }
    RequestId next_poll_proposable = 0;

    void executed(RequestId const request_id) {
      pollable_below = request_id + window + 1;
    }

    ProcId id;

   private:
    void enqueueSignatureVerification(SignedRequest &&req,
                                      TailThreadPool::TaskQueue &tq) {
      auto const req_id = req.id();
      auto req_it = requests.find(req_id);
      if (req_it != requests.end() && req_it->second.getSignature()) {
        return;
      }
      #ifdef LATENCY_HOOKS
        hooks::sig_check_start = hooks::Clock::now();
      #endif
      tq.enqueue([this, req = std::move(req)]() mutable {
        auto [raw_req, sig] = req.split();
        // auto const ts = std::chrono::steady_clock::now();
        auto const valid = crypto.verify(sig, raw_req.rawBuffer().data(),
                                         raw_req.rawBuffer().size(), id);
        // fmt::print("Checking the signature took: {}\n",
        // std::chrono::steady_clock::now() - ts);
        verified_signatures.enqueue({std::move(raw_req), sig, valid});
      });
    }

    Crypto &crypto;
    third_party::sync::MpmcQueue<VerifiedSignature> verified_signatures;
    TailThreadPool::TaskQueue client_signature_verification;
    TailThreadPool::TaskQueue leader_signature_verification;
    size_t window;
    TailMap<RequestId, RequestStateMachine> requests;
    std::vector<TailQueue<Request>> buffered_requests;
    LOGGER_DECL_INIT(logger, "RpcClientRequestIngress");
  };

 public:
  RequestIngress(Crypto &crypto, TailThreadPool &thread_pool,
                 ProcId const min_client_id, ProcId const max_client_id,
                 size_t const window, size_t const unanimity_size)
      : crypto{crypto},
        thread_pool{thread_pool},
        min_client_id{min_client_id},
        unanimity_size{unanimity_size},
        window{window},
        clients{static_cast<size_t>(max_client_id - min_client_id + 1)} {}

  void tick() {
    for (auto &client : connected_clients) {
      client.get().pollVerifiedSignatures(unanimity_size);
    }
  }

  void fromFollower(Request &&req, size_t const follower_index) {
    getOrCreateClient(req.clientId())
        .fromFollower(std::move(req), follower_index);
  }

  void fromClient(Request &&req) {
    getOrCreateClient(req.clientId())
        .fromClient(std::move(req), unanimity_size);
  }

  void fromLeader(SignedRequest &&req) {
    getOrCreateClient(req.clientId()).fromLeader(std::move(req));
  }

  void fromClient(SignedRequest &&req) {
    getOrCreateClient(req.clientId()).fromClient(std::move(req));
  }

  /**
   * @brief Poll the next request that was received from a client.
   *
   * @return OptionalConstRef<Request>
   */
  OptionalConstRef<Request> pollReceived() {
    auto const nb_clients = connected_clients.size();
    if (unlikely(nb_clients == 0)) {
      return std::nullopt;
    }
    // TODO: Iterate efficiently over clients that have to say something
    for (size_t i = 0; i < nb_clients; i++) {
      auto const client_id = (next_client_poll_received + i) % nb_clients;
      auto &client = connected_clients.at(client_id);
      if (auto polled = client.get().pollReceived()) {
        next_client_poll_received = client_id + 1;
        return polled;
      }
    }
    return std::nullopt;
  }
  size_t next_client_poll_received = 0;

  /**
   * @brief Poll the next request that was not echoed to the leader.
   *
   * @param leader_index
   * @return OptionalConstRef<Request>
   */
  OptionalConstRef<Request> pollToEcho(size_t const leader_index) {
    auto const nb_clients = connected_clients.size();
    if (unlikely(nb_clients == 0)) {
      return std::nullopt;
    }
    // TODO: Iterate efficiently over clients that have to say something
    for (size_t i = 0; i < nb_clients; i++) {
      auto const client_idx = (next_client_poll_to_echo + i) % nb_clients;
      auto &client = connected_clients.at(client_idx);
      if (auto polled = client.get().pollToEcho(leader_index)) {
        next_client_poll_to_echo = client_idx + 1;
        return polled;
      }
    }
    return std::nullopt;
  }
  size_t next_client_poll_to_echo = 0;

  std::optional<std::pair<ConstRef<Request>, ConstRef<Crypto::Signature>>>
  pollToForward(size_t const dest_index) {
    auto const nb_clients = connected_clients.size();
    if (unlikely(nb_clients == 0)) {
      return std::nullopt;
    }
    // TODO: Iterate efficiently over clients that have to say something
    for (size_t i = 0; i < nb_clients; i++) {
      auto const client_idx = (next_client_poll_forward + i) % nb_clients;
      auto &client = connected_clients.at(client_idx);
      if (auto polled = client.get().pollToForward(dest_index)) {
        next_client_poll_forward = client_idx + 1;
        return polled;
      }
    }
    return std::nullopt;
  }
  size_t next_client_poll_forward = 0;

  OptionalConstRef<Request> pollProposable(bool const fast_path,
                                           bool const optimisitc) {
    auto const nb_clients = connected_clients.size();
    if (unlikely(nb_clients == 0)) {
      return std::nullopt;
    }
    // TODO: Iterate efficiently over clients that have to say something
    for (size_t i = 0; i < nb_clients; i++) {
      auto const client_idx = (next_client_poll_echoed + i) % nb_clients;
      auto &client = connected_clients.at(client_idx);
      if (auto polled = client.get().pollProposable(fast_path, optimisitc)) {
        next_client_poll_echoed = client_idx + 1;
        return polled;
      }
    }
    return std::nullopt;
  }
  size_t next_client_poll_echoed = 0;

  void executed(ProcId const client_id, RequestId const request_id) {
    getOrCreateClient(client_id).executed(request_id);
  }

 private:
  ClientRequestIngress &getOrCreateClient(ProcId const client_id) {
    if (unlikely(!validClientId(client_id))) {
      throw std::runtime_error(fmt::format(
          "Byzantine behavior: Invalid invalid client id {}.", client_id));
    }

    // We create a client for that client if we have never heard of him.
    auto &client = getClient(client_id);
    if (unlikely(!client)) {
      crypto.fetchPublicKey(client_id);
      client.emplace(crypto, thread_pool, client_id, window,
                     unanimity_size - 1);
      connected_clients.push_back(*client);
    }
    return *client;
  }

  inline bool validClientId(ProcId const client_id) {
    return client_id >= min_client_id &&
           (client_id - min_client_id) < static_cast<ProcId>(clients.size());
  }

  inline std::optional<ClientRequestIngress> &getClient(
      ProcId const client_id) {
    return clients[client_id - min_client_id];
  }

  Crypto &crypto;
  TailThreadPool &thread_pool;
  ProcId const min_client_id;
  size_t const unanimity_size;
  size_t const window;
  std::vector<std::optional<ClientRequestIngress>> clients;
  std::vector<internal::Ref<ClientRequestIngress>> connected_clients;

  size_t start_idx = 0;

  LOGGER_DECL_INIT(logger, "RpcRequestIngress");
};
}  // namespace dory::ubft::rpc::internal
