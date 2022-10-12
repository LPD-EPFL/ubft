#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <dory/conn/rc.hpp>
#include <dory/shared/branching.hpp>

#include "../../buffer.hpp"
#include "header.hpp"
#include "lazy.hpp"
#include "sync-sender.hpp"

namespace dory::ubft::tail_p2p::internal {

/**
 * @brief A Sender abstraction that provides tail validity and always gives a
 * slot.
 *
 * The pipeline is as follows:
 * 1) A buffer where to write the message is obtained from a pool via `getSlot`,
 * 2) The buffer is put on a `being_written` queue and given to the user,
 * 3) The user marks all buffers obtained via `getSlot` as being ready via
 * `send` which puts the buffers in the tail queue, 4) On every tick, the
 * abstraction tries to move as many buffers from the tail to the underlying
 * Sender abstraction, 5) Upon successful utilization of the underlying Sender
 * abstraction, the buffer is freed (put back in the pool).
 *
 * Tail validity is only ensured after a call to `send`.
 * Reason: Messages that are being written and have not been sent yet reduce the
 * space of the tail.
 */
class AsyncSender : public Lazy {
 public:
  size_t static constexpr bufferSize(size_t const tail,
                                     size_t const max_msg_size) {
    return SyncSender::bufferSize(tail, max_msg_size);
  }

  AsyncSender(size_t const tail, size_t const max_msg_size,
              conn::ReliableConnection &&rc)
      : buffer_pool{tail, max_msg_size},
        sender{tail, max_msg_size, std::move(rc)} {}

  /**
   * @brief Get a slot/buffer where to write a message.
   *
   * @param size (in bytes) of the message to write.
   * @return void* the buffer where to write.
   */
  void *getSlot(Size size) {
    // 0) Push as many slots as possible from the tail buffer to the underlying
    // abstraction,
    pushToSender();

    // 1) Try to give a slot from the underlying Sender directly,
    auto opt_slot = sender.getSlot(size);
    if (likely(opt_slot)) {
      return *opt_slot;
    }

    // 2) If no slot is available, push into the being_written buffer.
    if (auto opt_buffer = buffer_pool.take(size)) {
      being_written.emplace_back(std::move(*opt_buffer));
      return being_written.back().data();
    }

    // 3) If the tail buffer is full, recycle the oldest slot in tail.
    if (tail_buffer.empty()) {
      throw std::runtime_error(
          "Called getSlot too many times without calling send.");
    }
    being_written.push_back(std::move(tail_buffer.front()));
    tail_buffer.pop_front();
    being_written.back().resize(size);
    return being_written.back().data();
  }

  /**
   * @brief Mark all slots previously provided by `getSlot` as being ready to be
   * forwarded to the underlying abstraction.
   *
   */
  inline void send() {
    sender.send();
    pushToTailBuffer();
    pushToSender();
  }

  inline void tick() {
    sender.tick();
    pushToSender();
  }

  inline void tickEvery(size_t const calls) {
    if (unlikely(++calls_to_tick_every >= calls)) {
      tick();
      calls_to_tick_every = 0;
    }
  }

 private:
  inline void pushToTailBuffer() {
    while (unlikely(!being_written.empty())) {
      tail_buffer.push_back(std::move(being_written.front()));
      being_written.pop_front();
    }
  }

  void pushToSender() {
    while (unlikely(!tail_buffer.empty())) {
      auto &buffer = tail_buffer.front();
      auto opt_slot = sender.getSlot(static_cast<Header::Size>(buffer.size()));
      if (unlikely(!opt_slot)) {
        break;
      }
      std::memcpy(*opt_slot, buffer.data(), buffer.size());
      tail_buffer.pop_front();
      sender.send();
    }
  }

  Pool buffer_pool;
  std::deque<Buffer> being_written;
  std::deque<Buffer> tail_buffer;
  SyncSender sender;

  size_t calls_to_tick_every = 0;
};

}  // namespace dory::ubft::tail_p2p::internal
