#pragma once

#include <dory/crypto/hash/blake3.hpp>
#include <dory/shared/move-indicator.hpp>

#include "buffer.hpp"

namespace dory::ubft {

class Message {
 public:
  Message(Buffer &&buffer) : buffer{std::move(buffer)} {}

  Message(Message &&) = default;
  Message &operator=(Message &&) = default;

  bool operator==(Message const &o) const { return buffer == o.buffer; }
  bool operator!=(Message const &o) const { return !(*this == o); }

  Buffer &rawBuffer() { return buffer; }
  Buffer const &rawBuffer() const { return buffer; }

  crypto::hash::Blake3Hash hash() const {
    return crypto::hash::blake3(buffer.cbegin(), buffer.cend());
  }

  /**
   * @brief Take the buffer out of the message, leaving it in a destroyable
   *        state.
   *
   * @return Buffer
   */
  Buffer takeBuffer() { return std::move(buffer); }

  MoveIndicator moved;

 private:
  Buffer buffer;
};

}  // namespace dory::ubft
