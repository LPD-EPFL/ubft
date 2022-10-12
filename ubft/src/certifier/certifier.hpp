#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/core.h>
#include <xxhash.h>
#include <hipony/enumerate.hpp>

#include <dory/conn/rc.hpp>
#include <dory/crypto/hash/blake2b.hpp>
#include <dory/shared/assert.hpp>
#include <dory/shared/dynamic-bitset.hpp>
#include <dory/shared/logger.hpp>
#include <dory/shared/match.hpp>
#include <dory/shared/optimistic-find.hpp>
#include <dory/third-party/sync/mpmc.hpp>

#include "../buffer.hpp"
#include "../crypto.hpp"
// #include "../tail-queue.hpp"
#include "../tail-map/tail-map.hpp"
#include "../tail-p2p/receiver.hpp"
#include "../tail-p2p/sender.hpp"
#include "../thread-pool/tail-thread-pool.hpp"
#include "../types.hpp"
#include "../unsafe-at.hpp"
#include "certificate.hpp"
#include "internal/share-message.hpp"
#include "types.hpp"

namespace dory::ubft::certifier {

class Certifier {
  auto static constexpr SlowPathEnabled = true;

  using Hash = crypto::hash::Blake3Hash;
  using Share = internal::ShareMessage;

  struct ComputedShare {
    Share::BufferLayout share;
    Buffer buffer;  // So that it is returned in the main thread.
  };

  struct VerifiedShare {
    size_t replica;
    Share share;
    bool valid;
  };

 public:
  Certifier(Crypto &crypto, TailThreadPool &thread_pool, size_t const tail,
            size_t const max_msg_size, std::string const &str_identifier,
            std::vector<tail_p2p::AsyncSender> &&promise_senders,
            std::vector<tail_p2p::Receiver> &&promise_receivers,
            std::vector<tail_p2p::AsyncSender> &&share_senders,
            std::vector<tail_p2p::Receiver> &&share_receivers)
      : crypto{crypto},
        tail{tail},
        str_identifier{str_identifier},
        identifier{XXH64(str_identifier.data(), str_identifier.size(), 0)},
        promise_senders{std::move(promise_senders)},
        promise_receivers{std::move(promise_receivers)},
        share_senders{std::move(share_senders)},
        share_receivers{std::move(share_receivers)},
        // tail queued and stored, in the thread pool, 1 for slack
        buffer_pool{
            2 * tail +
                TailThreadPool::TaskQueue::maxOutstanding(tail, thread_pool) +
                1,
            max_msg_size},
        // for each share source, we remember and queue tail shares + in the
        // thread pool + 1 for slack
        share_buffer_pool{
            (this->share_receivers.size() + 1) *
                    (2 * tail + TailThreadPool::TaskQueue::maxOutstanding(
                                    tail, thread_pool)) +
                1,
            Share::BufferSize},
        msg_tail{tail},
        sorted_computed_shares{tail},
        // queued_share_computations{tail},
        share_computation_task_queue{thread_pool, tail} {
    always_assert(
        ("All vectors should be the same size.",
         this->promise_senders.size() == this->promise_receivers.size() &&
             this->promise_receivers.size() == this->share_senders.size() &&
             this->share_senders.size() == this->share_receivers.size()));
    for (auto const &_ : this->share_receivers) {
      check_share_task_queues.emplace_back(thread_pool, tail);
    }

    for (auto const &_ : this->promise_receivers) {
      buffered_promises.emplace_back();
      buffered_shares.emplace_back();
    }
  }

  void tick() {
    if (likely(msg_tail.empty())) {
      return;
    }

    // Fast path
    if (shouldRunFastPath()) {
      pollPromises();
      for (auto &sender : promise_senders) {
        sender.tickForCorrectness();
      }
    }
    // Slow path
    if (unlikely(shouldRunSlowPath())) {
      if (likely((++ticks % 16) != 0)) {  // The slow path runs every 16 ticks.
        return;
      }
      pollShares();
      // Should it be moved above for correctness?
      for (auto &sender : share_senders) {
        sender.tickForCorrectness();
      }
      pollComputedShares();
      offloadShareComputation();
      pollVerifiedShares();
    }
  }

  void acknowledge(Index const index, uint8_t const *const begin,
                   uint8_t const *const end,
                   bool const implicit_promise = false) {
    auto it_ok = msg_tail.tryEmplace(index, identifier, index, begin, end,
                                     share_receivers.size());
    if (!it_ok.second) {
      throw std::runtime_error("Acknowledged messages out of order.");
    }
    auto &msg_data = it_ok.first->second;
    // Fast path
    if (shouldRunFastPath()) {
      // The promise can be implicit, e.g., if the acknowledgement is triggered
      // upon receiption of one's message. In this case, to reduce latency, the
      // promise can be omitted but the other receivers must call
      // receivedImplicitPromise(source, index).
      if (!implicit_promise) {
        for (auto &sender : promise_senders) {
          *reinterpret_cast<Index *>(sender.getSlot(sizeof(Index))) = index;
          sender.send();
        }
      }
      // We replay buffered promises.
      for (auto &&[replica, promises] : hipony::enumerate(buffered_promises)) {
        // We first drop all the useless promises,
        while (!promises.empty() && promises.front() < index) {
          promises.pop_front();
        }
        // and only proceed if there is one that matches our index.
        if (promises.empty() || promises.front() != index) {
          continue;
        }
        promises.pop_front();
        if (!msg_data.receivedPromise(replica)) {
          throw std::logic_error(
              "Unimplemented: Byzantine behavior, promised twice.");
        }
      }
    }
    // Slow path
    // We replay buffered shares.
    for (auto &&[replica, shares] : hipony::enumerate(buffered_shares)) {
      // We first drop all the useless shares.
      while (!shares.empty() && shares.front().msgIndex() < index) {
        shares.pop_front();
      }
      // and only proceed if there is one that matches our index.
      if (shares.empty() || shares.front().msgIndex() != index) {
        continue;
      }
      enqueueShareVerification(std::move(shares.front()), replica);
      shares.pop_front();
    }

    // We don't want to compute the hash in the main thread as it can be
    // expensive. Yet, we need to extend the lifetime of the message.

    // We empty useless computed shares to release buffers.
    while (computed_shares.size_approx() + 1 > tail) {
      std::optional<ComputedShare> popped;
      computed_shares.try_dequeue(popped);
    }

    if (queued_share_computations.size() + 1 > tail) {
      queued_share_computations.pop_front();
    }

    auto opt_buffer = buffer_pool.take(end - begin);
    if (unlikely(!opt_buffer)) {
      throw std::logic_error(
          fmt::format("[{}] Ran out of free buffers for share computation.",
                      str_identifier));
    }
    std::copy(begin, end, opt_buffer->data());

    queued_share_computations.emplace_back(index, std::move(*opt_buffer));
  }

  void receivedImplicitPromise(ProcId from, Index const index) {
    auto it = optimistic_find_front(msg_tail, index);
    if (unlikely(it == msg_tail.end())) {
      return;
    }
    for (auto &&[replica, receiver] : hipony::enumerate(promise_receivers)) {
      if (receiver.procId() == from) {
        it->second.receivedPromise(replica);
        return;
      }
    }
    throw std::runtime_error(fmt::format("Replica {} not found", from));
  }

  std::optional<Index> pollPromise() {
    if (unlikely(msg_tail.empty())) {
      return std::nullopt;
    }
    next_promise = std::max(next_promise, msg_tail.begin()->first);
    auto it = msg_tail.find(next_promise);
    if (it == msg_tail.end() || !it->second.pollablePromise()) {
      return std::nullopt;
    }
    return next_promise++;
  }

  std::optional<Certificate> pollCertificate() {
    if (unlikely(msg_tail.empty())) {
      return std::nullopt;
    }
    next_certificate = std::max(next_certificate, msg_tail.begin()->first);
    auto it = msg_tail.find(next_certificate);
    if (it == msg_tail.end() || !it->second.pollableCertificate()) {
      return std::nullopt;
    }
    auto certificate = it->second.buildCertificate(*this);
    next_certificate++;
    return certificate;
  }

  bool check(Certificate const &certificate) const {
    if (certificate.identifier() != identifier) {
      fmt::print("Identifiers don't match: {} vs {}\n.",
                 certificate.identifier(), identifier);
      return false;
    }
    auto const quorum = (share_receivers.size() + 1) / 2 + 1;

    // We check that the certificate has the correct number of shares.
    if (certificate.nbShares() != quorum) {
      // fmt::print("shares: {}, expected: {}\n", nbShares(), quorum);
      return false;
    }
    // We check that all shares come from different emitters.
    std::unordered_set<ProcId> seen;
    for (size_t i = 0; i < certificate.nbShares(); i++) {
      if (!seen.insert(certificate.share(i).emitter).second) {
        LOGGER_WARN(logger, "Byzantine certificate with 2+ shares from {}",
                    certificate.share(i).emitter);
        return false;
      }
    }
    // We delay hashing to the moment where it's absolutely required.
    std::optional<crypto::hash::Blake3Hash> hash;
    // We check that each signature is valid
    for (size_t i = 0; i < certificate.nbShares(); i++) {
      auto const &share = certificate.share(i);
      auto const &[signer, sig] = share;
      // To speed the certificate verification time up, we check if the share
      // we used the share ourself (and thus already verified it).
      if (signatureVerified(certificate.index(), signer, share.signature)) {
        continue;
      }
      // We compute the hash if it wasn't done before.
      if (!hash) {
        auto hasher = crypto::hash::blake3_init();
        crypto::hash::blake3_update(hasher, certificate.identifier());
        crypto::hash::blake3_update(hasher, certificate.index());
        crypto::hash::blake3_update(
            hasher, certificate.message(),
            certificate.message() + certificate.messageSize());
        hash = crypto::hash::blake3_final(hasher);
      }
      if (!crypto.verify(sig, hash->data(), hash->size(), signer)) {
        // fmt::print("Bad signature from {}\n", signer);
        // fmt::print("Checking against: {}\n", hash);
        // fmt::print("With the following signature: {}\n", sig);
        return false;
      }
    }
    return true;
  }

  /**
   * @brief Check if a share was already verified when building certificates.
   *
   * This is used to speedup certificate verification.
   *
   * @return true if it was use and still is in tail
   * @return false if it could not be found
   */
  bool signatureVerified(Index const idx, ProcId const source,
                         Crypto::Signature const &signature) const {
    auto const it = msg_tail.find(idx);
    if (it == msg_tail.end()) {
      return false;
    }
    return it->second.signatureVerified(*this, source, signature);
  }

  void toggleFastPath(bool const enable) { run_fast_path = enable; }

  void toggleSlowPath(bool const enable) { run_slow_path = enable; }

  /**
   * @brief Drop references to previously aknowledged messages up to index.
   *
   * @param index
   */
  void forgetMessages(std::optional<Index> const index = std::nullopt) {
    while (!msg_tail.empty() && (!index || msg_tail.begin()->first <= *index)) {
      msg_tail.popFront();
    }
  }

  /**
   * @brief Generates an unverifiable certificate which is solely trusted thanks
   *        to its special value. It is essentially used as the place holder
   *        for a certificate which has a default value.
   *
   * @tparam T
   * @param value
   * @return Certificate
   */
  template <typename T>
  Certificate genesisCertificate(T const &value) {
    auto const *const begin = reinterpret_cast<uint8_t const *>(&value);
    return Certificate(identifier, 0, {}, begin, begin + sizeof(T));
  }

 private:
  void pollPromises() {
    for (auto &&[replica, receiver] : hipony::enumerate(promise_receivers)) {
      Index polled_index;
      auto const polled = receiver.poll(&polled_index);
      if (!polled) {
        continue;
      }
      handlePromise(polled_index, replica);
    }
  }

  void handlePromise(Index const index, size_t const replica) {
    // fmt::print("Received a promise on index {} from {}.\n", index, replica);
    auto md_it = optimistic_find_front(msg_tail, index);
    // If we haven't acknowledged the message ourselves, we buffer the promise.
    if (md_it == msg_tail.end()) {
      auto &promise_buffer = buffered_promises[replica];
      if (!promise_buffer.empty() && promise_buffer.back() >= index) {
        throw std::logic_error(
            "Unimplemented: Byzantine behavior, promises sent out of order.");
      }
      promise_buffer.push_back(index);
      if (promise_buffer.size() > tail) {
        promise_buffer.pop_front();
      }
      return;
    }
    if (!md_it->second.receivedPromise(replica)) {
      throw std::logic_error(
          "Unimplemented: Byzantine behavior, promised twice.");
    }
  }

  void offloadShareComputation() {
    if (likely(queued_share_computations.empty())) {
      return;
    }
    for (auto &[i, buffer] : queued_share_computations) {
      auto const index = i;  // structured bindings cannot be captured
      sorted_computed_shares.tryEmplace(index);  // Where to store the share.
      share_computation_task_queue.enqueue(
          [this, index, buffer = std::move(buffer)]() mutable {
            auto acc = crypto::hash::blake3_init();
            crypto::hash::blake3_update(acc, identifier);
            crypto::hash::blake3_update(acc, index);
            crypto::hash::blake3_update(acc, buffer.cbegin(), buffer.cend());
            auto hash = crypto::hash::blake3_final(acc);
            auto sign = crypto.sign(hash.data(), hash.size());
            computed_shares.enqueue(
                ComputedShare{{index, sign}, std::move(buffer)});
          });
    }
    queued_share_computations.clear();
  }

  void pollShares() {
    for (auto &&[r, receiver] : hipony::enumerate(share_receivers)) {
      auto opt_buffer = share_buffer_pool.borrowNext();
      if (!opt_buffer) {
        throw std::logic_error("No share buffer available.");
      }
      auto const polled = receiver.poll(opt_buffer->get().data());
      if (!polled) {
        continue;
      }
      auto share = Share::tryFrom(*share_buffer_pool.take(*polled));
      auto &replica = r;  // bug: structured bindings cannot be captured.
      match{share}([&](std::invalid_argument e) { throw e; },
                   [&](Share &sm) { handleShare(std::move(sm), replica); });
    }
  }

  void handleShare(Share &&share, size_t const replica) {
    auto md_it = optimistic_find_front(msg_tail, share.msgIndex());
    // We only check the share if we acknowledged the message ourselves.
    if (md_it != msg_tail.end()) {
      enqueueShareVerification(std::move(share), replica);
      return;
    }
    // Otherwise we buffer it
    auto &shares = buffered_shares[replica];
    if (!shares.empty() && shares.back().msgIndex() >= share.msgIndex()) {
      LOGGER_ERROR(logger,
                   "[{}] Byzantine behavior, {} sent shares out of order.",
                   str_identifier, replica);
      throw std::logic_error("Unexpected Byzantine behavior.");
    }
    if (shares.size() + 1 > tail) {
      shares.pop_front();
    }
    shares.emplace_back(std::move(share));
  }

  void pollComputedShares() {
    // We first sort the shares by inserting them in a sorted map and only then
    // do we send them (in order, without gaps).
    std::optional<ComputedShare> computed_share;
    // try_dequeue does not use the optional, hence the need for manual reset.
    while (
        (computed_share.reset(), computed_shares.try_dequeue(computed_share))) {
      auto const index = computed_share->share.index;
      auto it = sorted_computed_shares.find(index);
      // The share is too outdated and is now useless.
      if (it == sorted_computed_shares.end()) {
        continue;
      }
      it->second.swap(computed_share);
    }

    // Now we broadcast as many shares as possible in order.
    while (!msg_tail.empty() && !sorted_computed_shares.empty()) {
      auto const &first_msg = *msg_tail.begin();
      auto const &first_share = *sorted_computed_shares.begin();
      // We pop shares that are useless
      if (first_share.first < first_msg.first) {
        sorted_computed_shares.popFront();
        continue;
      }
      // And we stop when a share is missing in order
      if (!first_share.second) {
        return;
      }
      // Otherwise we broadcast it
      for (auto &sender : share_senders) {
        auto *const slot = reinterpret_cast<Share::BufferLayout *>(
            sender.getSlot(Share::BufferSize));
        *slot = first_share.second->share;
        sender.send();
      }
      auto opt_buffer = share_buffer_pool.take(Share::BufferSize);
      if (!opt_buffer) {
        throw std::runtime_error("Ran out of share buffers to poll shares.");
      }
      *reinterpret_cast<Share::BufferLayout *>(opt_buffer->data()) =
          first_share.second->share;
      auto share = std::get<Share>(Share::tryFrom(std::move(*opt_buffer)));
      auto const my_index = share_receivers.size();
      auto const valid = true;
      my_shares.emplace_back(VerifiedShare{my_index, std::move(share), valid});
      sorted_computed_shares.popFront();
    }
  }

  void enqueueShareVerification(Share &&share, size_t const replica) {
    auto const index = share.msgIndex();
    auto const &hash = optimistic_find_front(msg_tail, index)->second.hash();
    uat(check_share_task_queues, replica)
        .enqueue(
            [this, share = std::move(share), replica, hash = hash]() mutable {
              auto valid =
                  crypto.verify(share.signature(), hash.data(), hash.size(),
                                uat(share_receivers, replica).procId());
              verified_shares.enqueue(
                  VerifiedShare{replica, std::move(share), valid});
            });
  }

  void pollVerifiedShares() {
    std::optional<VerifiedShare> verified_share;
    // try_dequeue does not use the optional, hence the need for manual reset.
    while (
        (verified_share.reset(), verified_shares.try_dequeue(verified_share))) {
      handleVerifiedShare(*verified_share);
    }
    while (unlikely(!my_shares.empty())) {
      handleVerifiedShare(my_shares.front());
      my_shares.pop_front();
    }
  }

  inline void handleVerifiedShare(VerifiedShare &verified_share) {
    if (!verified_share.valid) {
      LOGGER_ERROR(
          logger,
          "[{}] Byzantine Behavior: Received invalid share #{} from {}.",
          str_identifier, verified_share.share.msgIndex(),
          verified_share.replica);
      throw std::logic_error("Unexpected Byzantine behavior.");
    }
    auto replica = verified_share.replica;
    auto &share = verified_share.share;
    auto md_it = optimistic_find_front(msg_tail, share.msgIndex());
    // We discard shares that are not usefull anymore.
    if (md_it == msg_tail.end()) {
      return;
    }
    // We note that we received a share.
    if (!md_it->second.receivedShare(replica, std::move(share))) {
      throw std::logic_error(
          "Unimplemented: Byzantine behavior, share sent twice.");
    }
  }

  bool shouldRunFastPath() const { return run_fast_path; }

  bool run_fast_path = true;

  bool shouldRunSlowPath() const { return run_slow_path; }

  bool run_slow_path = false;

  Crypto &crypto;
  size_t const tail;
  std::string const str_identifier;
  Identifier const identifier;
  std::vector<tail_p2p::AsyncSender> promise_senders;
  std::vector<tail_p2p::Receiver> promise_receivers;
  std::vector<tail_p2p::AsyncSender> share_senders;
  std::vector<tail_p2p::Receiver> share_receivers;

  class MessageData {
   public:
    MessageData(Identifier const identifier, Index const index,
                uint8_t const *const begin, uint8_t const *const end,
                size_t const other_replicas)
        : identifier{identifier},
          index{index},
          begin{begin},
          end{end},
          other_replicas{other_replicas},
          promised{other_replicas} {}

    /**
     * @brief Computes the to-be-signed hash and memoizes it.
     *
     * @return hash::Hash const&
     */
    Hash const &hash() {
      if (!computed_hash) {
        auto acc = crypto::hash::blake3_init();
        crypto::hash::blake3_update(acc, identifier);
        crypto::hash::blake3_update(acc, index);
        crypto::hash::blake3_update(acc, begin, end);
        computed_hash = crypto::hash::blake3_final(acc);
      }
      return *computed_hash;
    }

    bool receivedPromise(size_t replica_index) {
      return promised.set(replica_index);
    }

    bool receivedShare(size_t const replica_index, Share &&checked_share) {
      return received_shares
          .try_emplace(replica_index, std::move(checked_share))
          .second;
    }

    bool pollablePromise() const { return promised.full(); }

    bool pollableCertificate() const {
      return received_shares.size() >= (other_replicas + 1) / 2 + 1;
    }

    Certificate buildCertificate(Certifier const &certifier) const {
      if (!pollableCertificate()) {
        throw std::logic_error("Trying to build a non-pollable certificate.");
      }
      std::vector<std::pair<ProcId, Crypto::Signature const &>> signatures;
      auto const &receivers = certifier.share_receivers;
      for (auto const &[replica, share] : received_shares) {
        auto const source = replica < receivers.size()
                                ? receivers[replica].procId()
                                : certifier.crypto.myId();
        signatures.emplace_back(source, share.signature());
        // No need for more than a quorum of shares.
        if (signatures.size() == (other_replicas + 1) / 2 + 1) {
          break;
        }
      }
      return Certificate(identifier, index, signatures, begin, end);
    }

    bool signatureVerified(Certifier const &certifier, ProcId const source,
                           Crypto::Signature const &sig) const {
      auto const &receivers = certifier.share_receivers;
      for (auto const &[some_replica_index, some_share] : received_shares) {
        auto const some_source = some_replica_index < receivers.size()
                                     ? receivers[some_replica_index].procId()
                                     : certifier.crypto.myId();
        if (some_source == source) {
          return sig == some_share.signature();
        }
      }
      return false;
    }

    Identifier const identifier;
    Index const index;
    uint8_t const *const begin;
    uint8_t const *const end;
    std::optional<Hash> computed_hash;
    size_t const other_replicas;
    std::map<size_t, Share> received_shares;
    DynamicBitset promised;
  };

  Pool buffer_pool;
  Pool share_buffer_pool;  // Must be destroyed after the tail.
  TailMap<Index, MessageData> msg_tail;
  TailMap<Index, std::optional<ComputedShare>> sorted_computed_shares;
  std::vector<std::deque<Index>> buffered_promises;
  std::vector<std::deque<Share>> buffered_shares;
  Index next_promise = 0;
  Index next_certificate = 0;
  size_t ticks = 0;
  third_party::sync::MpmcQueue<ComputedShare> computed_shares;
  // TODO(Antoine): have a per-replica queue.
  third_party::sync::MpmcQueue<VerifiedShare> verified_shares;
  std::deque<VerifiedShare> my_shares;
  // TailQueue<std::pair<Index, Buffer>> queued_share_computations;
  std::deque<std::pair<Index, Buffer>> queued_share_computations;
  TailThreadPool::TaskQueue share_computation_task_queue;
  std::vector<TailThreadPool::TaskQueue> check_share_task_queues;
  LOGGER_DECL_INIT(logger, "Certifier");
};

}  // namespace dory::ubft::certifier
