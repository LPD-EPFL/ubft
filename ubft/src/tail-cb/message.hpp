#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <variant>

#include "../message.hpp"

namespace dory::ubft::tail_cb {

class Message : public ubft::Message {
 public:
  using Index = size_t;

  struct Header {
    Index index;
  };

  static_assert(sizeof(Header) == sizeof(Index),
                "The Header struct is not packed. Use "
                "`__attribute__((__packed__))` to pack it");

  struct __attribute__((__packed__)) BufferLayout {
    Header header;
    uint8_t data;
  };

  static_assert(sizeof(BufferLayout) == sizeof(Header) + sizeof(uint8_t),
                "The BufferLayout struct is not packed. Use "
                "`__attribute__((__packed__))` to pack it");

  static constexpr size_t bufferSize(size_t const msg_size) {
    return sizeof(Header) + msg_size;
  }

 protected:
  Message(Buffer &&buffer) : ubft::Message(std::move(buffer)) {
    if (rawBuffer().size() < sizeof(Header)) {
      throw std::logic_error(
          "The size of the provided buffer is smaller than the size of the "
          "buffer. Should have been checked before.");
    }
  }

 public:
  Buffer::ConstIterator cbegin() const {
    return rawBuffer().cbegin() + offsetof(BufferLayout, data);
  }
  Buffer::ConstIterator cend() const { return rawBuffer().cend(); }
  uint8_t const *data() const {
    return reinterpret_cast<uint8_t const *>(&*cbegin());
  }

  size_t size() const {
    return static_cast<size_t>(rawBuffer().size() - sizeof(Header));
  }

  Header const &header() const {
    return *reinterpret_cast<Header const *>(&*rawBuffer().cbegin());
  }

  Index index() const { return header().index; }

  static std::variant<std::invalid_argument, Message> tryFrom(Buffer &&buffer) {
    if (buffer.size() < sizeof(Header)) {
      return std::invalid_argument("Buffer is smaller than Header.");
    }
    return Message(std::move(buffer));
  }
};

}  // namespace dory::ubft::tail_cb
