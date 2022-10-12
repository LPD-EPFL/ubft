#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <vector>

#include <fmt/core.h>
#include <xxhash.h>

#include <dory/conn/rc.hpp>
#include <dory/ctrl/block.hpp>
#include <dory/shared/branching.hpp>

#include "header.hpp"
#include "lazy.hpp"

namespace dory::ubft::tail_p2p::internal {

class CircularBuffer {
  using Index = size_t;

 public:
  CircularBuffer(size_t const nb_elems, uintptr_t const buffer_start,
                 size_t const buffer_len, size_t const slot_size)
      : nb_elems{nb_elems},
        buffer_start{buffer_start},
        buffer_len{buffer_len},
        slot_size{slot_size} {
    if (buffer_len < nb_elems * slot_size) {
      throw std::runtime_error(
          fmt::format("Buffer too small: {} given, {} required.", buffer_len,
                      nb_elems * slot_size));
    }
  }

  std::optional<uintptr_t> acquire() {
    if (next_alloc == next_free + nb_elems) {
      return std::nullopt;
    }
    auto const index = next_alloc++;
    return buffer_start + slot_size * (index % nb_elems);
  }

  void release() {
    if (next_alloc == next_free) {
      throw std::runtime_error("Cannot release faster than alloc.");
    }
    next_free++;
  }

 private:
  size_t const nb_elems;
  uintptr_t const buffer_start;
  size_t const buffer_len;
  size_t const slot_size;

  Index next_alloc = 0;
  Index next_free = 0;
};

/**
 * @brief A Sender abstraction that provides tail validity but may not give a
 * slot if there are outstanding messages.
 *
 * The pipeline is as follows:
 * 1) A buffer where to write the message is obtained from a pool via `getSlot`,
 * 2) The user marks all buffers obtained via `getSlot` as being ready via
 * `send`, 3) On every tick, the abstraction tries to RDMA-write messages, 4)
 * The buffer is freed upon write completion.
 *
 * Tail validity is only ensured after a call to `send`.
 * Reason: Messages that are being written and have not been sent yet reduce the
 * space of the tail.
 */
class SyncSender : public Lazy {
  static size_t constexpr MaxOutstandingWrites =
      conn::ReliableConnection::WrDepth;
  static_assert(MaxOutstandingWrites <= ctrl::ControlBlock::CqDepth);

 public:
  size_t static constexpr bufferSize(size_t const tail,
                                     size_t const max_msg_size) {
    return tail * slotSize(max_msg_size);
  }

  inline static size_t constexpr slotSize(size_t const max_msg_size) {
    auto const unaligned_size = sizeof(Header) + max_msg_size;
    // Slots are 8-byte aligned so that reading fields from their header is
    // atomic.
    return (unaligned_size + 8 - 1) & static_cast<size_t>(-8);
  }

  SyncSender(size_t const tail, size_t const max_msg_size,
             conn::ReliableConnection &&rc)
      : tail{tail},
        slot_size{slotSize(max_msg_size)},  // todo: align
        buffer{tail, rc.getMr().addr, rc.getMr().size, slot_size},
        rc{std::move(rc)} {
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
    wcs.reserve(MaxOutstandingWrites);
  }

  inline void tick() {
    // We want the tick to be as inexpensive as possible when there is nothing
    // to do. Especially, we don't want to call pollcq.
    if (unlikely(outstanding_writes != 0)) {
      // poll
      wcs.resize(outstanding_writes);
      if (unlikely(!rc.pollCqIsOk(conn::ReliableConnection::SendCq, wcs))) {
        throw std::runtime_error("Error while polling CQ.");
      }
      // release
      for (auto const &wc : wcs) {
        if (wc.status != IBV_WC_SUCCESS) {
          // TODO(Antoine): consider the guy as being dead or, for stubborness,
          // re-post the WRITE.
          throw std::runtime_error(
              fmt::format("Error in RDMA WRITE: {}", wc.status));
        }
        buffer.release();
        outstanding_writes--;
      }
    }
    // push
    pushToQp();
  }

  /**
   * @brief Get a slot/buffer where to write a message. If no buffer is
   * available, returns nullopt.
   *
   * @param size (in bytes) of the message to write.
   * @return std::optional<void *> the buffer where to write the message if
   * available.
   */
  inline std::optional<void *> getSlot(Size size) {
    if (unlikely(size > slot_size)) {
      throw std::runtime_error(fmt::format(
          "p2p slot size {} is smaller than requested {}.", slot_size, size));
    }
    auto const full_slot = buffer.acquire();
    if (unlikely(!full_slot)) {
      return std::nullopt;
    }
    auto *const header = reinterpret_cast<Header *>(*full_slot);
    header->incarnation =
        static_cast<Header::Incarnation>(next_slot++ / tail + 1);
    header->size = size;
    auto *const data = reinterpret_cast<void *>(*full_slot + sizeof(Header));
    to_send.push_back(reinterpret_cast<void *>(*full_slot));
    return data;
  }

  /**
   * @brief Mark all slots previously provided by `getSlot` as being ready to be
   * sent over RDMA.
   *
   */
  void send() {
    send_before = next_slot;
    pushToQp();
  }

 private:
  inline void pushToQp() {
    while (unlikely(!to_send.empty()) && next_send < send_before &&
           outstanding_writes < conn::ReliableConnection::WrDepth) {
      auto *const slot = to_send.front();
      auto *const header = reinterpret_cast<Header *>(slot);
      auto *const data = reinterpret_cast<void *>(
          reinterpret_cast<uintptr_t>(slot) + sizeof(Header));
      header->hash = XXH3_64bits(data, header->size);
      uint32_t const full_size =
          static_cast<uint32_t>(sizeof(Header)) + header->size;
      if (!rc.postSendSingle(conn::ReliableConnection::RdmaWrite, 0, slot,
                             full_size,
                             rc.remoteBuf() + slot_size * (next_send % tail))) {
        // TODO(Antoine): consider the guy as being dead or, for stubborness,
        // re-establish the QP and the WRITE.
        throw std::runtime_error("Error while posting RDMA write.");
      }
      outstanding_writes++;
      to_send.pop_front();
      next_send++;
    }
  }

  std::deque<void *> to_send;
  size_t next_slot = 0;
  size_t send_before = 0;
  size_t next_send = 0;
  size_t outstanding_writes = 0;

  size_t const tail;
  size_t const slot_size;
  CircularBuffer buffer;
  conn::ReliableConnection rc;

  std::vector<struct ibv_wc> wcs;
};

}  // namespace dory::ubft::tail_p2p::internal
