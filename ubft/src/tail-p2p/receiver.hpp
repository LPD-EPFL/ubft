#pragma once

#ifndef __x86_64__
// This code relies on x86 Read-Write ordering guarantees and does not support
// other architectures.
#error "Only the x86 architecture is supported."
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <variant>

#include <fmt/core.h>
#include <xxhash.h>

#include <dory/conn/rc.hpp>
#include <dory/shared/branching.hpp>

#include "../types.hpp"
#include "internal/header.hpp"
#include "internal/sync-sender.hpp"

namespace dory::ubft::tail_p2p {

class Receiver {
  using Header = internal::Header;
  using Index = size_t;
  using MsgId = std::pair<Header::Incarnation, Index>;
  static MsgId constexpr FirstMsg = {1, 0};

 public:
  size_t static constexpr bufferSize(size_t const tail,
                                     size_t const max_msg_size) {
    return internal::SyncSender::bufferSize(tail, max_msg_size);
  }

  inline static size_t constexpr slotSize(size_t const max_msg_size) {
    return internal::SyncSender::slotSize(max_msg_size);
  }

  Receiver(size_t const tail, size_t const max_msg_size,
           conn::ReliableConnection &&rc)
      : tail{tail},
        slot_size{slotSize(max_msg_size)},
        rc{std::move(rc)},
        ptr_to_scan{msgPtr(FirstMsg)} {
    if (this->rc.getMr().size < bufferSize(tail, max_msg_size)) {
      throw std::runtime_error(
          fmt::format("Buffer is not large enough to store the tail: {} "
                      "required, {} given.",
                      bufferSize(tail, max_msg_size), this->rc.getMr().size));
    }
    if (this->rc.getMr().size != this->rc.remoteSize()) {
      throw std::runtime_error(
          fmt::format("Local and remote MR sizes do not match ({} vs {}).",
                      this->rc.getMr().size, this->rc.remoteSize()));
    }
    for (Index i = 0; i < tail; i++) {
      auto *const header = reinterpret_cast<Header *>(msgPtr({0, i}));
      header->incarnation = 0;
      header->size = 0;
    }
  }

  /**
   * @brief Poll the next received message in a given buffer.
   * This may return false even though a message is available if it has not been
   * polled.
   *
   * @param buffer where to poll the message.
   * @return the size of the message that was polled into the buffer if any.
   */
  std::optional<size_t> poll(void *buffer) {
    for (size_t i = 0; i < tail; i++) {
      auto const poll_result = tryPoll(buffer);
      if (unlikely(std::holds_alternative<Polled>(poll_result))) {
        return std::get<Polled>(poll_result).size;
      }
      if (likely(std::holds_alternative<Empty>(poll_result))) {
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  ProcId procId() const { return rc.procId(); }

 private:
  size_t const tail;
  size_t const slot_size;
  conn::ReliableConnection rc;

  struct Polled {
    size_t size;
  };
  struct Empty {};
  struct Straggling {};
  using TryPollResult = std::variant<Polled, Empty, Straggling>;

  TryPollResult tryPoll(void *buffer) {
    // Note:
    //   Write order is: H, I, S, D
    //   Read order is: I, (H, S, D), I

    auto const *const header = reinterpret_cast<Header volatile *>(ptr_to_scan);
    Header::Incarnation const scanned_incarnation = header->incarnation;

    if (scanned_incarnation < best_to_deliver.first) {
      return Empty{};  // No new message was written.
    }

    MsgId const scanning = {scanned_incarnation, next_to_scan.second};

    // Ensures that the incarnation number is read first, before the (hash,
    // size, data).
    __asm volatile("" ::: "memory");
    Header::Hash const hash = header->hash;
    Header::Size const size = header->size;

    // If the message we are looking for is there or if we are on a falling
    // edge, we would happily deliver it.
    if (scanning == best_to_deliver ||
        (max_scanned_straggling && *max_scanned_straggling > scanning)) {
      auto const *const data_beginning =
          reinterpret_cast<uint8_t volatile *>(ptr_to_scan + sizeof(Header));
      auto const *const data_end = reinterpret_cast<uint8_t volatile *>(
          ptr_to_scan + sizeof(Header) + size);
      auto *byte_buffer = reinterpret_cast<uint8_t *>(buffer);
      // memcpy does not work with volatile pointers.
      // Discarding the volatile qualifier should be safe here, to check.
      std::memcpy(byte_buffer, const_cast<uint8_t *>(data_beginning), size);

      // Ensures that the incarnation number is read after the (hash, size,
      // data).
      __asm volatile("" ::: "memory");

      if (scanning.first != header->incarnation) {
        // fmt::print("[receiver] the header incarnation was overwritten\n");
        return Straggling{};  // We verify that the memory was not
                              // overwritten.
      }
      if (hash == XXH3_64bits(buffer, size)) {
        // We compute the ID of the next message in the sequence of deliveries.
        // We hope to deliver it, but maybe there will be a gap and we will have
        // to deliver on a falling edge.
        next_to_scan = best_to_deliver = successor(scanning);
        ptr_to_scan = msgPtr(next_to_scan);
        return Polled{size};
      }
      // fmt::print("[receiver] hash didn't match\n");
      return Straggling{};  // Hash didn't match.
    }
    // fmt::print("[receiver] looking for a falling edge\n");
    max_scanned_straggling = scanning;
    next_to_scan = successor(scanning);
    ptr_to_scan = msgPtr(next_to_scan);
    return Straggling{};  // Looking for a falling edge.
  }

  std::optional<MsgId> max_scanned_straggling;
  MsgId next_to_scan = FirstMsg;
  uintptr_t ptr_to_scan;
  MsgId best_to_deliver = FirstMsg;

  inline MsgId successor(MsgId const &old_id) const {
    auto new_id = old_id;
    if (++new_id.second >= tail) {
      new_id.second = 0;
      new_id.first++;
    }
    return new_id;
  }

  inline uintptr_t msgPtr(MsgId const &id) {
    return rc.getMr().addr + slot_size * id.second;
  }
};

}  // namespace dory::ubft::tail_p2p
