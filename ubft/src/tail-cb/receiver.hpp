#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <variant>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <xxhash.h>
#include <hipony/enumerate.hpp>

#include <dory/conn/rc.hpp>
#include <dory/crypto/hash/blake3.hpp>
#include <dory/shared/branching.hpp>
#include <dory/shared/dynamic-bitset.hpp>
#include <dory/shared/match.hpp>
#include <dory/shared/optimistic-find.hpp>
#include <dory/shared/units.hpp>
#include <dory/shared/unused-suppressor.hpp>
#include <dory/third-party/sync/mpmc.hpp>

#include "../buffer.hpp"
#include "../crypto.hpp"
#include "../replicated-swmr/reader.hpp"
#include "../replicated-swmr/writer.hpp"
#include "../tail-p2p/receiver.hpp"
#include "../tail-p2p/sender.hpp"
#include "../thread-pool/tail-thread-pool.hpp"
#include "../types.hpp"
#include "../unsafe-at.hpp"

#include "../latency-hooks.hpp"

namespace dory::ubft::tail_cb {

class Receiver {
  auto static constexpr SlowPathEnabled = true;
  // When to switch from raw echo to hashed echo.
  auto static constexpr HashThreshold = units::kibibytes(8);
  using Hash = crypto::hash::Blake3Hash;
  auto static constexpr HashLength = crypto::hash::Blake3HashLength;

 public:
  using Index = Message::Index;
  using Size = tail_p2p::Size;

 private:
  using Signature = Crypto::Signature;
  using SignatureMessage = internal::SignatureMessage;

  auto static constexpr CustomIncarnationsEnabled = true;

  struct VerifiedSignature {
    Index index;
    bool ok;
    enum Origin { Broadcaster, ReceiverRegister } origin;
  };

  struct Register {
    crypto::hash::Blake3Hash hash;
    Signature signature;
  };

 public:
  static size_t constexpr maxEchoSize(size_t max_msg_size) {
    return Message::bufferSize(std::min(max_msg_size, HashThreshold - 1));
  }

  size_t static constexpr RegisterValueSize = sizeof(Register);

  Receiver(Crypto &crypto, TailThreadPool &thread_pool,
           const ProcId broadcaster_id, size_t const borrowed_messages,
           size_t const tail, size_t const max_msg_size,
           tail_p2p::Receiver &&message_receiver,
           tail_p2p::Receiver &&signature_receiver,
           std::vector<tail_p2p::Receiver> &&echo_receivers,
           std::vector<tail_p2p::AsyncSender> &&echo_senders,
           std::vector<replicated_swmr::Reader> &&swmr_readers,
           replicated_swmr::Writer &&swmr_writer)
      : crypto{crypto},
        broadcaster_id{broadcaster_id},
        tail{tail},
        message_receiver(std::move(message_receiver)),
        signature_receiver(std::move(signature_receiver)),
        echo_senders{std::move(echo_senders)},
        echo_receivers{std::move(echo_receivers)},
        swmr_writer{std::move(swmr_writer)},
        swmr_readers{std::move(swmr_readers)},
        message_buffer_pool{borrowed_messages + tail + 1,
                            Message::bufferSize(max_msg_size)},
        signature_buffer_pool{tail + 1, SignatureMessage::BufferSize},
        echo_buffer_pool{this->echo_receivers.size() * (tail + 1),
                         maxEchoSize(max_msg_size)},
        recv_check_task_queue{thread_pool, tail} {
    for (auto &_ : this->swmr_readers) {
      read_check_task_queues.emplace_back(thread_pool, tail);
    }
    always_assert(
        ("For each other receiver, we should have 1 p2p-sender, 1 p2p-receiver "
         "and 1 swmr-reader.",
         this->echo_receivers.size() == this->echo_senders.size() &&
             this->echo_senders.size() == this->swmr_readers.size()));

    for (auto &_ : this->echo_receivers) {
      buffered_echoes.emplace_back();
    }
  }

  void tick() {
    // We help others make progress, even if we delivered ourselves.
    for (auto &sender : echo_senders) {
      sender.tickForCorrectness();
    }

    // We poll messages from the broadcaster and only continue the tick if we
    // have something to deliver.
    pollBroadcasterMessage();
    if (msg_tail.empty()) {
      return;
    }

    // We will try to deliver it via echoes.
    pollEchoes();

    // Otherwise, if enabled, we will run the slow path.
    if (likely(!shouldRunSlowPath())) {
      return;
    }
    pollBroadcasterSignature();
    pollSignatureVerifications();
    swmr_writer.tick();
    pollWriteCompletions();
    for (auto &reader : swmr_readers) {
      reader.tick();
    }
    pollReadCompletions();
  }

  /**
   * @brief Poll a message if any is available.
   *        At most `tail` messages can be held by the upper-level abstraction.
   *
   * @return std::optional<Message>
   */
  std::optional<Message> poll() {
    if (msg_tail.empty() || !msg_tail.begin()->second.pollable()) {
      return std::nullopt;
    }
    // We bump the 'latest_polled_message' marker to enforce FIFO ordering.
    latest_polled_message = msg_tail.begin()->first;
    auto to_ret = msg_tail.begin()->second.extractMessage();
    // Pop the entry from the map;
    msg_tail.erase(msg_tail.begin());
    return to_ret;
  }

  void toggleSlowPath(bool const enable) {
    if (enable && !SlowPathEnabled) {
      throw std::runtime_error("Slow path was disabled at compilation.");
    }
    slow_path_on = enable;
  }

  ProcId procId() const { return message_receiver.procId(); }

  ProcId broadcasterId() const { return broadcaster_id; }

 private:
  void pollBroadcasterMessage() {
    auto opt_buffer = message_buffer_pool.borrowNext();
    if (unlikely(!opt_buffer)) {
      throw std::runtime_error("User is retaining all buffers in Messages.");
    }
    auto opt_polled = message_receiver.poll(opt_buffer->get().data());
    if (!opt_polled) {
      return;
    }
    auto msg = Message::tryFrom(*message_buffer_pool.take(*opt_polled));
    match{msg}(
        [](std::invalid_argument &e) {
          throw std::logic_error(fmt::format("Unimplemented: {}", e.what()));
        },
        [this](Message &m) { handleMessage(std::move(m)); });
  }

  void pollBroadcasterSignature() {
    auto opt_buffer = signature_buffer_pool.borrowNext();
    if (unlikely(!opt_buffer)) {
      throw std::logic_error("Error, buffers not recycled correctly.");
    }
    auto opt_polled = signature_receiver.poll(opt_buffer->get().data());
    if (!opt_polled) {
      return;
    }
    auto msg = internal::SignatureMessage::tryFrom(
        *signature_buffer_pool.take(*opt_polled));
    match{msg}(
        [](std::invalid_argument &e) {
          throw std::logic_error(fmt::format("Unimplemented: {}", e.what()));
        },
        [this](SignatureMessage &m) { handleSignature(std::move(m)); });
  }

  /**
   * @brief Poll echoes received from other receivers (via p2p).
   *
   */
  void pollEchoes() {
    for (auto &&[r, receiver] : hipony::enumerate(echo_receivers)) {
      auto opt_buffer = echo_buffer_pool.borrowNext();
      if (unlikely(!opt_buffer)) {
        throw std::logic_error("Error, buffers not recycled correctly.");
      }
      auto const polled = receiver.poll(opt_buffer->get().data());
      if (!polled) {
        continue;
      }
      auto echo = Message::tryFrom(*echo_buffer_pool.take(*polled));
      auto &replica = r;  // bug: structured bindings cannot be captured
      match{echo}(
          [&](std::invalid_argument &e) {
            fmt::print("Malformed echo from {}: {}.\n", replica, e.what());
          },
          [&](Message &m) { handleEcho(std::move(m), replica); });
    }
  }

  /**
   * @brief Handle a Data message (i.e., containing the actual cb-broadcast
   * message).
   *
   * @param message
   */
  void handleMessage(Message &&message) {
    auto const index = message.index();
    // fmt::print("Polled Message #{} from broadcaster (size = {})\n", index,
    //            message.size());
    // We create an entry in the tail_msg map to store data about this message.
    if (unlikely(pessimistic_find(msg_tail, index) != msg_tail.end() ||
                 latest_polled_message >= index)) {
      throw std::logic_error(
          "Unimplemented (Byzantine Broadcaster sent the same message more "
          "than once)!");
    }
    if (unlikely(!msg_tail.empty() && msg_tail.rbegin()->first >= index)) {
      fmt::print(
          "Message dropped as it was received out of order (Byzantine).\n");
      return;
    }
    auto &msg_data =
        msg_tail.try_emplace(index, std::move(message), echo_receivers.size())
            .first->second;
    if (msg_tail.size() > tail) {
      msg_tail.erase(msg_tail.begin());
    }

    // We replay all buffered echoes
    for (auto &&[replica, echo_buffer] : hipony::enumerate(buffered_echoes)) {
      while (unlikely(!echo_buffer.empty() &&
                      echo_buffer.front().index() < index)) {
        echo_buffer.pop_front();
      }
      if (unlikely(!echo_buffer.empty() &&
                   echo_buffer.front().index() == index)) {
        if (unlikely(!msg_data.echoed(replica, echo_buffer.front()))) {
          throw std::logic_error(
              "Unimplemented (Byzantine behavior, replica Echoed twice)!");
        }
        echo_buffer.pop_front();
      }
    }

    // We send all echoes
    for (auto &sender : echo_senders) {
      auto &message = msg_data.getMessage();
      if (likely(message.size() < HashThreshold)) {
        // If the message is small enough, we send a raw copy.
        const auto &raw_buffer = message.rawBuffer();
        auto *echo_buffer = reinterpret_cast<uint8_t *>(
            sender.getSlot(static_cast<Size>(raw_buffer.size())));
        std::copy(raw_buffer.cbegin(), raw_buffer.cend(), echo_buffer);
      } else {
        // Otherwise we send its hash.
        auto &echo_buffer = *reinterpret_cast<Message::BufferLayout *>(
            sender.getSlot(static_cast<Size>(Message::bufferSize(HashLength))));
        echo_buffer.header.index = message.index();
        *reinterpret_cast<Hash *>(&echo_buffer.data) = msg_data.hash();
      }
      sender.send();
    }
  }

  /**
   * @brief Handle an echo message.
   *
   * @param message
   */
  void handleEcho(Message &&echo, size_t const replica) {
    // fmt::print("Polled echo #{} from replica {}\n", echo.index, replica);
    // We discard echoes that aren't useful.
    if (latest_polled_message > echo.index() ||
        (!msg_tail.empty() && msg_tail.begin()->first > echo.index())) {
      return;
    }
    // If we already received the message, we take the echo into account.
    auto md_it = optimistic_find_front(msg_tail, echo.index());
    if (md_it != msg_tail.end()) {
      if (unlikely(!md_it->second.echoed(replica, echo))) {
        throw std::logic_error(
            "Unimplemented (Byzantine behavior, replica Echoed twice)!");
      }
      return;
    }
    // Otherwise, we buffer it.
    auto &echo_buffer = buffered_echoes[replica];
    if (unlikely(!echo_buffer.empty() &&
                 echo_buffer.back().index() > echo.index())) {
      throw std::logic_error(
          "Unimplemented (Byzantine behavior, Echoes sent out of order)!");
    }
    echo_buffer.emplace_back(std::move(echo));
    if (echo_buffer.size() > tail) {
      echo_buffer.pop_front();
    }
  }

  /**
   * @brief Handle a Signature message that should have been p2p-sent by the
   * broadcaster after the associated Data message.
   *
   * @param message
   */
  void handleSignature(SignatureMessage &&signature_message) {
    auto index = signature_message.index();
    auto msg_data_it = optimistic_find_front(msg_tail, index);

    // If the associated message is not in the tail anymore, the signature is
    // useless.
    if (msg_data_it == msg_tail.end()) {
      // We get back the buffer that was storing the signature.
      return;
    }
    auto &msg_data = msg_data_it->second;
    if (msg_data.hasSignature()) {
      throw std::logic_error(fmt::format(
          "Unimplemented (Byzantine Broadcaster {} sent the signature more "
          "than once)!",
          broadcaster_id));
    }

    msg_data.setSignature(std::move(signature_message));
    // We verify the signature in the background. Only after its verification
    // will we write it to our SWMR register.
    recv_check_task_queue.enqueue([this, index, hash = msg_data.hash(),
                                   signature = msg_data.getSignature()] {
      auto ok =
          crypto.verify(signature, hash.data(), hash.size(), broadcaster_id);

      verified_signatures.enqueue({index, ok, VerifiedSignature::Broadcaster});
    });
  }

  /**
   * @brief Poll the completion of signature verifications that were running in
   * in the thread pool.
   *
   */
  void pollSignatureVerifications() {
    VerifiedSignature verified_signature;
    while (verified_signatures.try_dequeue(verified_signature)) {
      auto [index, ok, origin] = verified_signature;
      // fmt::print("In pollSignatureVerifications, index: {}, ok: {}, origin:
      // {}\n", index, ok, origin);
      auto md_it = optimistic_find_front(msg_tail, index);
      // If the associated message is not in the tail anymore, the signature is
      // useless.
      if (md_it == msg_tail.end()) {
        continue;
      }
      auto &msg_data = md_it->second;
      switch (origin) {
        case VerifiedSignature::Broadcaster: {
          // If a signature comes from the broadcaster, it should be valid.
          #ifdef LATENCY_HOOKS
            hooks::swmr_write_start = hooks::Clock::now();
          #endif
          if (!ok) {
            throw std::logic_error(fmt::format(
                "Unimplemented: Byzantine broadcaster {} sent an invalid "
                "signature for {}.",
                broadcaster_id, index));
          }
          // We can now write the received signature to our SWMR.
          auto swmr_index = index % tail;
          if (outstanding_writes.find(swmr_index) != outstanding_writes.end()) {
            throw std::logic_error(
                "Unimplemented: recycled swmr before completion of the "
                "previous WRITE.");
          }
          auto opt_slot = swmr_writer.getSlot(swmr_index);
          if (!opt_slot) {
            throw std::logic_error(
                "Called getSlot before the previous WRITE completed.");
          }
          auto &slot = *reinterpret_cast<Register *>(*opt_slot);
          slot.hash = msg_data.hash();
          slot.signature = msg_data.getSignature();
          auto const incarnation = index / tail + 1;
          swmr_writer.write(swmr_index, incarnation);
          outstanding_writes.try_emplace(swmr_index, index);

          // We will only proceed to read the other SWMRs when the write
          // completes.
          break;
        }
        case VerifiedSignature::ReceiverRegister: {
          // Signatures found in a receiver's SWMR are only checked if they
          // do not match the one received directly from the broadcaster.
          // In this case, a valid signature implies an equivocation.
          if (ok) {
            throw std::logic_error(
                fmt::format("Unimplemented: Byzantine broadcaster {} "
                            "equivocated for index {}.",
                            broadcaster_id, index));
          }
          // We mark this receiver as being safe from equivocation.
          msg_data.checkedAReceiver();
          break;
        }
        default:
          throw std::runtime_error("Uncaught switch statement case");
      }
    }
  }

  void pollWriteCompletions() {
    // We iterate over the map of reads while removing its elements.
    for (auto it = outstanding_writes.cbegin(); it != outstanding_writes.cend();
         /* in body */) {
      auto const [swmr_index, index] = *it;
      if (!swmr_writer.completed(swmr_index)) {
        ++it;
        continue;
      }
      #ifdef LATENCY_HOOKS
        hooks::swmr_write_latency.addMeasurement(hooks::Clock::now() - hooks::swmr_write_start);
      #endif
      it = outstanding_writes.erase(it);
      // If the message is not in the tail anymore, we discard the WRITE.
      auto md_it = optimistic_find_front(msg_tail, index);
      if (md_it == msg_tail.end()) {
        continue;
      }
      // Otherwise, we enqueue READs.
      auto &vec = outstanding_reads.try_emplace(index).first->second;
      #ifdef LATENCY_HOOKS
        hooks::swmr_read_start = hooks::Clock::now();
      #endif
      for (auto &reader : swmr_readers) {
        vec.emplace_back(reader.read(swmr_index));
      }
    }
  }

  void pollReadCompletions() {
    // We iterate over the map of reads while removing its elements.
    for (auto it = outstanding_reads.begin(); it != outstanding_reads.end();
         /* in body */) {
      auto [index, opt_job_handles] = *it;
      size_t completed_reads = 0;
      for (auto &&[replica, swmr_reader] : hipony::enumerate(swmr_readers)) {
        // We fetch the handle for this specific replica.
        auto &opt_job_handle = opt_job_handles[replica];
        // If the (optional) handle is empty, then it already completed.
        if (!opt_job_handle) {
          completed_reads++;
          continue;
        }
        // Otherwise, we check its completion.
        auto opt_polled = swmr_reader.poll(*opt_job_handle);
        if (!opt_polled) {
          continue;
        }
        auto const expected_incarnation = index / tail + 1;
        if (opt_polled->second > expected_incarnation) {
          throw std::logic_error(
              fmt::format("Unimplemented: SWMR was recycled: incarnation {} "
                          "found, {} expected.",
                          opt_polled->second, expected_incarnation));
        }
        completed_reads++;
        opt_job_handle.reset();
        // If the message is not in the tail anymore, we discard the READ.
        auto md_it = optimistic_find_front(msg_tail, index);
        if (md_it == msg_tail.end()) {
          continue;
        }
        // Otherwise, we compare the read signature against the one we received.
        auto &signature =
            reinterpret_cast<Register *>(opt_polled->first.get())->signature;
        auto &msg_data = md_it->second;
        // if it is the same, then the receiver is "safe".
        if (opt_polled->second < expected_incarnation ||
            msg_data.signatureMatches(signature)) {
          msg_data.checkedAReceiver();
        } else {
          // Otherwise, someone acted Byzantine, we need to determine who it is.
          uat(read_check_task_queues, replica)
              .enqueue([this, index = index,
                        buffer = std::move(opt_polled->first)]() {
                auto const &reg = *reinterpret_cast<Register *>(buffer.get());
                auto ok = crypto.verify(reg.signature, reg.hash.data(),
                                        reg.hash.size(), broadcaster_id);
                verified_signatures.enqueue(
                    {index, ok, VerifiedSignature::ReceiverRegister});
              });
        }
      }
      if (completed_reads == swmr_readers.size()) {
        it = outstanding_reads.erase(it);
        #ifdef LATENCY_HOOKS
          hooks::swmr_read_latency.addMeasurement(hooks::Clock::now() - hooks::swmr_read_start);
        #endif
      } else {
        ++it;
      }
    }
  }

  inline bool shouldRunSlowPath() const {
    return SlowPathEnabled && slow_path_on;
  }

  bool slow_path_on = false;

  Crypto &crypto;
  ProcId const broadcaster_id;
  size_t const tail;

  // Receivers for messages and signature from the broadcaster
  tail_p2p::Receiver message_receiver;
  tail_p2p::Receiver signature_receiver;

  // Echo to everyone the message from the broadcaster
  std::vector<tail_p2p::AsyncSender> echo_senders;

  // Receive the echoes from everyone
  std::vector<tail_p2p::Receiver> echo_receivers;

  // Write the messages with is (verified) signature to your indestructible
  // register
  replicated_swmr::Writer swmr_writer;

  // Scan the indestructible registers of others
  std::vector<replicated_swmr::Reader> swmr_readers;

  class MessageData {
   public:
    MessageData(Message &&message, size_t const other_receivers)
        : message{std::move(message)},
          other_receivers{other_receivers},
          echoes{other_receivers} {}

    /**
     * @brief Mark this message as having been echoed.
     *
     * @param replica that echoed the message.
     * @param echo the echo message.
     * @return true if it is the first time this replica echoed the message,
     * @return false otherwise.
     */
    bool echoed(size_t const replica, Message const &echo) {
      // If the message is small enough, we expect to have received a raw copy.
      if (likely(message.size() < HashThreshold)) {
        if (unlikely(message != echo)) {
          fmt::print("Messages didn't match.\n");
          echoes_match = false;
        }
        return echoes.set(replica);
      }
      // Otherwise, we expect to have received a hash.
      if (unlikely(echo.size() != HashLength)) {
        fmt::print("Echo size does not matches a hash.\n");
        echoes_match = false;
        return echoes.set(replica);
      }
      auto const &hsh = *reinterpret_cast<Hash const *>(echo.data());
      if (unlikely(hsh != hash())) {
        fmt::print("Received hash did not match.\n");
        echoes_match = false;
      }
      return echoes.set(replica);
    }

    bool hasSignature() const { return signature.has_value(); }

    /**
     * @brief Set the Signature object.
     *
     * @param sgn
     * @return true if it is the first time the signature is set,
     * @return false otherwise.
     */
    bool setSignature(internal::SignatureMessage &&sgn) {
      if (signature) {
        return false;
      }
      signature.emplace(std::move(sgn));
      return true;
    }

    bool signatureMatches(Signature const &sign) const {
      return signature && sign == signature->signature();
    }

    Signature const &getSignature() const {
      if (!signature) {
        throw std::logic_error("Cannot get the signature before receiving it.");
      }
      return signature->signature();
    }

    void checkedAReceiver() {
      checked_receivers++;
      // TODO(Ant.): improve with echoes: a process that echoed does not need to
      // be further checked.
    }

    bool pollable() const {
      return (echoes.full() && echoes_match) ||     // Fast Path
             checked_receivers == other_receivers;  // Slow path
    }

    Message const &getMessage() const { return message; }

    Message extractMessage() {
      if (message.moved) {
        throw std::logic_error("The message was already moved.");
      }
      return std::move(message);
    }

    Hash const &hash() {
      if (!computed_hash) {
        computed_hash.emplace(message.hash());
      }
      return *computed_hash;
    }

   private:
    Message message;                    // Message itself
    std::optional<Hash> computed_hash;  // Message's hash.
    size_t const other_receivers;
    DynamicBitset echoes;  // Echoes received on this message
    bool echoes_match = true;

    std::optional<internal::SignatureMessage>
        signature;  // Signature received from the broadcaster
    size_t checked_receivers = 0;
  };

  Pool message_buffer_pool;
  Pool signature_buffer_pool;
  Pool echo_buffer_pool;

  std::map<Index, MessageData> msg_tail;
  std::optional<Index> latest_polled_message;
  std::vector<std::deque<Message>> buffered_echoes;

  third_party::sync::MpmcQueue<VerifiedSignature>
      verified_signatures;  // TODO(Antoine): define a max depth?
  // Map: Index to the register in the array of registers that I own -> The
  // index of the CB message (i.e., k).
  std::map<replicated_swmr::Writer::Index, Index> outstanding_writes;

  // Map: Index of the CB message -> The job handle for each register in the
  // register arrays owned from all the others. The job handle is optional to
  // mark the read as completed.
  std::map<Index,
           std::vector<std::optional<replicated_swmr::Reader::JobHandle>>>
      outstanding_reads;  // TODO(Antoine): limit growth?

  TailThreadPool::TaskQueue recv_check_task_queue;
  std::vector<TailThreadPool::TaskQueue> read_check_task_queues;
};

}  // namespace dory::ubft::tail_cb
